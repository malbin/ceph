
#include "common/debug.h"
#include "common/Formatter.h"

#include "CrushWrapper.h"

#define dout_subsys ceph_subsys_crush


void CrushWrapper::find_roots(set<int>& roots) const
{
  for (unsigned i=0; i<crush->max_rules; i++) {
    crush_rule *r = crush->rules[i];
    if (!r)
      continue;
    for (unsigned j=0; j<r->len; j++) {
      if (r->steps[j].op == CRUSH_RULE_TAKE)
	roots.insert(r->steps[j].arg1);
    }
  }
}


int CrushWrapper::remove_item(CephContext *cct, int item)
{
  ldout(cct, 5) << "remove_item " << item << dendl;

  crush_bucket *was_bucket = 0;
  int ret = -ENOENT;

  for (int i = 0; i < crush->max_buckets; i++) {
    if (!crush->buckets[i])
      continue;
    crush_bucket *b = crush->buckets[i];

    for (unsigned i=0; i<b->size; ++i) {
      int id = b->items[i];
      if (id == item) {
	if (item < 0) {
	  crush_bucket *t = get_bucket(item);
	  if (t && t->size) {
	    ldout(cct, 1) << "remove_device bucket " << item << " has " << t->size << " items, not empty" << dendl;
	    return -ENOTEMPTY;
	  }	    
	  was_bucket = t;
	}
	adjust_item_weight(cct, item, 0);
	ldout(cct, 5) << "remove_device removing item " << item << " from bucket " << b->id << dendl;
	crush_bucket_remove_item(b, item);
	ret = 0;
      }
    }
  }

  if (was_bucket) {
    ldout(cct, 5) << "remove_device removing bucket " << item << dendl;
    crush_remove_bucket(crush, was_bucket);
  }
  if (item >= 0 && name_map.count(item)) {
    name_map.erase(item);
    have_rmaps = false;
    ret = 0;
  }
  
  return ret;
}

bool CrushWrapper::check_item_loc(CephContext *cct, int item, map<string,string>& loc,
				  int *weight)
{
  ldout(cct, 5) << "check_item_loc item " << item << " loc " << loc << dendl;

  for (map<int,string>::const_iterator p = type_map.begin(); p != type_map.end(); p++) {
    // ignore device
    if (p->first == 0)
      continue;

    // ignore types that aren't specified in loc
    if (loc.count(p->second) == 0) {
      ldout(cct, 2) << "warning: did not specify location for '" << p->second << "' level (levels are "
		    << type_map << ")" << dendl;
      continue;
    }

    if (!name_exists(loc[p->second].c_str())) {
      ldout(cct, 5) << "check_item_loc bucket " << loc[p->second] << " dne" << dendl;
      return false;
    }

    int id = get_item_id(loc[p->second].c_str());
    if (id >= 0) {
      ldout(cct, 5) << "check_item_loc requested " << loc[p->second] << " for type " << p->second
		    << " is a device, not bucket" << dendl;
      return false;
    }

    crush_bucket *b = get_bucket(id);
    assert(b);

    // see if item exists in this bucket
    for (unsigned j=0; j<b->size; j++) {
      if (b->items[j] == item) {
	ldout(cct, 2) << "check_item_loc " << item << " exists in bucket " << b->id << dendl;
	if (weight)
	  *weight = crush_get_bucket_item_weight(b, j);
	return true;
      }
    }
    return false;
  }
  
  ldout(cct, 1) << "check_item_loc item " << item << " loc " << loc << dendl;
  return false;
}

/*
 * get the fully qualified location of a device by successively finding
 * parents beginning at ID and ending at highest type number specified in
 * the CRUSH map which assumes that if device foo is under device bar, the
 * type_id of foo < bar where type_id is the integer specified in the CRUSH map
 *
 * returns the location in the form of (type=foo) where type is a type of bucket
 * specified in the CRUSH map and foo is a name specified in the CRUSH map
 */
map<string, string> CrushWrapper::get_full_location(int id){

  map<string,string> full_location;
  pair<string, string> parent_coord;
  parent_coord = get_immediate_parent(id);
  int parent_id;

  /// read the type map and get the name of the type with the largest ID
  int high_type = 0;
  for (map<int, string>::iterator it = type_map.begin(); it != type_map.end(); it++){
    if ( (*it).first > high_type )
      high_type = (*it).first;
  }

  string high_type_name = type_map[high_type];

  full_location[ parent_coord.first ] = parent_coord.second;
  parent_id = get_item_id( (parent_coord.second).c_str() );


  while (parent_coord.first != high_type_name) {
    parent_coord = get_immediate_parent(parent_id);
    full_location[ parent_coord.first ] = parent_coord.second;
    if ( parent_coord.first != high_type_name ){
      parent_id = get_item_id( (parent_coord.second).c_str() );
    }
  }

  return full_location;
}


map<int, string> CrushWrapper::get_parent_hierarchy(int id)
{
  map<int,string> parent_hierarchy;
  pair<string, string> parent_coord = get_immediate_parent(id);
  int parent_id;

  // get the integer type for id and create a counter from there
  int type_counter = get_bucket_type(id);

  // if we get a negative type then we can assume that we have an OSD
  // change behavior in get_item_type FIXME
  if (type_counter < 0)
    type_counter = 0;

  // read the type map and get the name of the type with the largest ID
  int high_type = 0;
  for (map<int, string>::iterator it = type_map.begin(); it != type_map.end(); it++){
    if ( (*it).first > high_type )
      high_type = (*it).first;
  }

  parent_id = get_item_id((parent_coord.second).c_str());

  while (type_counter < high_type) {
    type_counter++;
    parent_hierarchy[ type_counter ] = parent_coord.first;

    if (type_counter < high_type){
      // get the coordinate information for the next parent
      parent_coord = get_immediate_parent(parent_id);
      parent_id = get_item_id(parent_coord.second.c_str());
    }
  }

  return parent_hierarchy;
}



int CrushWrapper::insert_item(CephContext *cct, int item, float weight, string name,
			      map<string,string>& loc)  // typename -> bucketname
{

  // create a look-up table in order to create random tags later
  static const char letter_pool[] =
     "0123456789"
     "abcdefghijklmnopqrstuvwxyz"
     "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

  // set how long random tags should be
  const static int appended_tag_length = 4;

  char random_tag[appended_tag_length + 2];

  int high_type = 0;
  int empty_weight = 0;
  int desired_type_id;
  int parent_id;
  int r;
  vector<int> created_bucket_ids;
  crush_bucket *b;

  // create an iterator over the location passed in
  map<string,string>::iterator it = loc.begin();
  string current_name = (*it).second;

  // assume that the bucket we are creating is one level below the desired location, I hate assumptions...
  if (!have_rmaps)
    build_rmap(type_map, type_rmap);

  // get the type of bucket at the desired placement location
  desired_type_id = type_rmap[ (*it).first ];
  int assumed_bucket_type_id =  desired_type_id -1;


  if (!name_exists( current_name.c_str()) && type_map.count(desired_type_id)){

    // if we have to create a series of buckets, the first item we are adding is actually a bucket
    if (assumed_bucket_type_id > 0 ) {
      int empty_items;
      add_bucket(item, CRUSH_BUCKET_STRAW, CRUSH_HASH_DEFAULT, assumed_bucket_type_id, 1, &empty_items, &empty_weight);
    }

    // just in case there is something weird going on in the type_map
    for (map<int,string>::iterator l = type_map.begin(); l != type_map.end(); l++){
      if ( (*l).first > high_type )
        high_type = (*l).first;
    }

    // calculate how many buckets we need to make between the desired_location and root
    int distance_to_root = high_type - assumed_bucket_type_id;

    if (distance_to_root > 1){
      int current_item = item;
      for (int current_type = desired_type_id; current_type < high_type; current_type++){

        // create a random tag to append to our otherwise boring name
        for (int i = 1; i <= appended_tag_length; i++){
          random_tag[i] = letter_pool[ rand() % (sizeof(letter_pool) -1)];
        }
        random_tag[appended_tag_length + 1] = 0;
        random_tag[0] = '-';

        // create a bucket
        ldout(cct, 5) << "insert_item creating bucket " << current_name << dendl;
        parent_id = add_bucket(0, CRUSH_BUCKET_STRAW, CRUSH_HASH_DEFAULT, current_type, 1, &current_item, &empty_weight);
        created_bucket_ids.push_back( parent_id );
        set_item_name(parent_id, current_name.c_str() );

        // we're naming the parent bucket here, so the current_name is actually of the next type
        current_name = (type_map[current_type + 1]+random_tag); // poor choice of variable name FIXME

        // get the actual bucket we just created
        b = get_bucket( parent_id );

        ldout(cct, 5) << "insert_item adding " << current_item
                      << " to bucket " << parent_id << dendl;
        r = crush_bucket_add_item(b, current_item, 0);
        assert(!r);
        current_item = parent_id;

      }

      // now add the last bucket we created to the root
      // for now assume that the root is named default, but we might want to check this someday, or give some sort of error
      int root_id = name_rmap["default"];
      b = get_bucket( root_id );

      ldout(cct, 5) << "insert_item adding " << parent_id
                    << " to bucket " << root_id << dendl;
      r = crush_bucket_add_item(b, parent_id, 0);
      assert(!r);

    }

    // now that we've added the (0-weighted) item and any parent buckets, adjust the weight.
    adjust_item_weightf(cct, item, weight);

    // since we added the item we wanted along with any and all parent buckets, we can probably just quit
    return 0;
  }

  ldout(cct, 5) << "insert_item item " << item << " weight " << weight
		<< " name " << name << " loc " << loc << dendl;

  if (name_exists(name.c_str())) {
    ldout(cct, 1) << "error: device name '" << name << "' already exists as id "
		  << get_item_id(name.c_str()) << dendl;
    return -EEXIST;
  }

  set_item_name(item, name.c_str());

  int cur = item;

  for (map<int,string>::iterator p = type_map.begin(); p != type_map.end(); p++) {
    // ignore device type
    if (p->first == 0)
      continue;

    // skip types that are unspecified
    if (loc.count(p->second) == 0) {
      ldout(cct, 2) << "warning: did not specify location for '" << p->second << "' level (levels are "
		    << type_map << ")" << dendl;
      continue;
    }

    if (!name_exists(loc[p->second].c_str())) {
      ldout(cct, 5) << "insert_item creating bucket " << loc[p->second] << dendl;
      int empty = 0;
      cur = add_bucket(0, CRUSH_BUCKET_STRAW, CRUSH_HASH_DEFAULT, p->first, 1, &cur, &empty);
      set_item_name(cur, loc[p->second].c_str());
      continue;
    }

    // add to an existing bucket
    int id = get_item_id(loc[p->second].c_str());
    if (!bucket_exists(id)) {
      ldout(cct, 1) << "insert_item doesn't have bucket " << id << dendl;
      return -EINVAL;
    }

    crush_bucket *b = get_bucket(id);
    assert(b);

    // make sure the item doesn't already exist in this bucket
    for (unsigned j=0; j<b->size; j++)
      if (b->items[j] == cur) {
	ldout(cct, 1) << "insert_item " << cur << " already exists in bucket " << b->id << dendl;
	return -EEXIST;
      }
    
    ldout(cct, 5) << "insert_item adding " << cur << " weight " << weight
		  << " to bucket " << id << dendl;
    int r = crush_bucket_add_item(b, cur, 0);
    assert (!r);

    // now that we've added the (0-weighted) item and any parent buckets, adjust the weight.
    adjust_item_weightf(cct, item, weight);

    if (item >= crush->max_devices) {
      crush->max_devices = item + 1;
      ldout(cct, 5) << "insert_item max_devices now " << crush->max_devices << dendl;
    }

    return 0;
  }

  ldout(cct, 1) << "error: didn't find anywhere to add item " << item << " in " << loc << dendl;
  return -EINVAL;
}

int CrushWrapper::move_bucket(CephContext *cct, int id, map<string,string>& loc)
{
  // sorry this only works for buckets
  if (id >= 0)
    return -EINVAL;

  if (!item_exists(id))
    return -ENOENT;

  // get the name of the bucket we are trying to move for later
  string id_name = get_item_name(id);

  // detach the bucket
  int bucket_weight = detach_bucket(cct, id);

  // un-set the device name so we can use add_item later
  build_rmap(name_map, name_rmap);
  name_map.erase(id);
  name_rmap.erase(id_name);

  // insert the bucket back into the hierarchy
  return insert_item(cct, id, bucket_weight / (float)0x10000, id_name, loc);
}


int CrushWrapper::update_item(CephContext *cct, int item, float weight, string name,
			      map<string,string>& loc)  // typename -> bucketname
{
  ldout(cct, 5) << "update_item item " << item << " weight " << weight
		<< " name " << name << " loc " << loc << dendl;
  int ret = 0;

  // compare quantized (fixed-point integer) weights!  
  int iweight = (int)(weight * (float)0x10000);
  int old_iweight;
  if (check_item_loc(cct, item, loc, &old_iweight)) {
    ldout(cct, 5) << "update_item " << item << " already at " << loc << dendl;
    if (old_iweight != iweight) {
      ldout(cct, 5) << "update_item " << item << " adjusting weight "
		    << ((float)old_iweight/(float)0x10000) << " -> " << weight << dendl;
      adjust_item_weight(cct, item, iweight);
      ret = 1;
    }
    if (get_item_name(item) != name) {
      ldout(cct, 5) << "update_item setting " << item << " name to " << name << dendl;
      set_item_name(item, name.c_str());
      ret = 1;
    }
  } else {
    if (item_exists(item)) {
      remove_item(cct, item);
    }
    ldout(cct, 5) << "update_item adding " << item << " weight " << weight
		  << " at " << loc << dendl;
    ret = insert_item(cct, item, weight, name.c_str(), loc);
    if (ret == 0)
      ret = 1;  // changed
  }
  return ret;
}

int CrushWrapper::adjust_item_weight(CephContext *cct, int id, int weight)
{
  ldout(cct, 5) << "adjust_item_weight " << id << " weight " << weight << dendl;
  for (int bidx = 0; bidx < crush->max_buckets; bidx++) {
    crush_bucket *b = crush->buckets[bidx];
    if (b == 0)
      continue;
    for (unsigned i = 0; i < b->size; i++)
      if (b->items[i] == id) {
	int diff = crush_bucket_adjust_item_weight(b, id, weight);
	ldout(cct, 5) << "adjust_item_weight " << id << " diff " << diff << dendl;
	adjust_item_weight(cct, -1 - bidx, b->weight);
	return 0;
      }
  }
  return -ENOENT;
}

bool CrushWrapper::check_item_present(int id)
{
  bool found = false;

  for (int bidx = 0; bidx < crush->max_buckets; bidx++) {
    crush_bucket *b = crush->buckets[bidx];
    if (b == 0)
      continue;
    for (unsigned i = 0; i < b->size; i++)
      if (b->items[i] == id)
	found = true;
  }
  return found;
}


pair<string,string> CrushWrapper::get_immediate_parent(int id)
{
  pair <string, string> loc;


  for (int bidx = 0; bidx < crush->max_buckets; bidx++) {
    crush_bucket *b = crush->buckets[bidx];
    if (b == 0)
      continue;
    for (unsigned i = 0; i < b->size; i++)
      if (b->items[i] == id){
        string parent_id = name_map[b->id];
        string parent_bucket_type = type_map[b->type];
        loc = make_pair(parent_bucket_type, parent_id);
      }
  }


  return loc;
}



void CrushWrapper::reweight(CephContext *cct)
{
  set<int> roots;
  find_roots(roots);
  for (set<int>::iterator p = roots.begin(); p != roots.end(); p++) {
    if (*p >= 0)
      continue;
    crush_bucket *b = get_bucket(*p);
    ldout(cct, 5) << "reweight bucket " << *p << dendl;
    int r = crush_reweight_bucket(crush, b);
    assert(r == 0);
  }
}

void CrushWrapper::encode(bufferlist& bl, bool lean) const
{
  assert(crush);

  __u32 magic = CRUSH_MAGIC;
  ::encode(magic, bl);

  ::encode(crush->max_buckets, bl);
  ::encode(crush->max_rules, bl);
  ::encode(crush->max_devices, bl);

  // buckets
  for (int i=0; i<crush->max_buckets; i++) {
    __u32 alg = 0;
    if (crush->buckets[i]) alg = crush->buckets[i]->alg;
    ::encode(alg, bl);
    if (!alg)
      continue;

    ::encode(crush->buckets[i]->id, bl);
    ::encode(crush->buckets[i]->type, bl);
    ::encode(crush->buckets[i]->alg, bl);
    ::encode(crush->buckets[i]->hash, bl);
    ::encode(crush->buckets[i]->weight, bl);
    ::encode(crush->buckets[i]->size, bl);
    for (unsigned j=0; j<crush->buckets[i]->size; j++)
      ::encode(crush->buckets[i]->items[j], bl);

    switch (crush->buckets[i]->alg) {
    case CRUSH_BUCKET_UNIFORM:
      ::encode(((crush_bucket_uniform*)crush->buckets[i])->item_weight, bl);
      break;

    case CRUSH_BUCKET_LIST:
      for (unsigned j=0; j<crush->buckets[i]->size; j++) {
	::encode(((crush_bucket_list*)crush->buckets[i])->item_weights[j], bl);
	::encode(((crush_bucket_list*)crush->buckets[i])->sum_weights[j], bl);
      }
      break;

    case CRUSH_BUCKET_TREE:
      ::encode(((crush_bucket_tree*)crush->buckets[i])->num_nodes, bl);
      for (unsigned j=0; j<((crush_bucket_tree*)crush->buckets[i])->num_nodes; j++)
	::encode(((crush_bucket_tree*)crush->buckets[i])->node_weights[j], bl);
      break;

    case CRUSH_BUCKET_STRAW:
      for (unsigned j=0; j<crush->buckets[i]->size; j++) {
	::encode(((crush_bucket_straw*)crush->buckets[i])->item_weights[j], bl);
	::encode(((crush_bucket_straw*)crush->buckets[i])->straws[j], bl);
      }
      break;

    default:
      assert(0);
      break;
    }
  }

  // rules
  for (unsigned i=0; i<crush->max_rules; i++) {
    __u32 yes = crush->rules[i] ? 1:0;
    ::encode(yes, bl);
    if (!yes)
      continue;

    ::encode(crush->rules[i]->len, bl);
    ::encode(crush->rules[i]->mask, bl);
    for (unsigned j=0; j<crush->rules[i]->len; j++)
      ::encode(crush->rules[i]->steps[j], bl);
  }

  // name info
  ::encode(type_map, bl);
  ::encode(name_map, bl);
  ::encode(rule_name_map, bl);

  // tunables
  ::encode(crush->choose_local_tries, bl);
  ::encode(crush->choose_local_fallback_tries, bl);
  ::encode(crush->choose_total_tries, bl);
}

void CrushWrapper::decode(bufferlist::iterator& blp)
{
  create();

  __u32 magic;
  ::decode(magic, blp);
  if (magic != CRUSH_MAGIC)
    throw buffer::malformed_input("bad magic number");

  ::decode(crush->max_buckets, blp);
  ::decode(crush->max_rules, blp);
  ::decode(crush->max_devices, blp);

  try {
    // buckets
    crush->buckets = (crush_bucket**)calloc(1, crush->max_buckets * sizeof(crush_bucket*));
    for (int i=0; i<crush->max_buckets; i++) {
      decode_crush_bucket(&crush->buckets[i], blp);
    }

    // rules
    crush->rules = (crush_rule**)calloc(1, crush->max_rules * sizeof(crush_rule*));
    for (unsigned i = 0; i < crush->max_rules; ++i) {
      __u32 yes;
      ::decode(yes, blp);
      if (!yes) {
	crush->rules[i] = NULL;
	continue;
      }

      __u32 len;
      ::decode(len, blp);
      crush->rules[i] = (crush_rule*)calloc(1, crush_rule_size(len));
      crush->rules[i]->len = len;
      ::decode(crush->rules[i]->mask, blp);
      for (unsigned j=0; j<crush->rules[i]->len; j++)
	::decode(crush->rules[i]->steps[j], blp);
    }

    // name info
    ::decode(type_map, blp);
    ::decode(name_map, blp);
    ::decode(rule_name_map, blp);
    build_rmaps();

    // tunables
    if (!blp.end()) {
      ::decode(crush->choose_local_tries, blp);
      ::decode(crush->choose_local_fallback_tries, blp);
      ::decode(crush->choose_total_tries, blp);
    }
    finalize();
  }
  catch (...) {
    crush_destroy(crush);
    throw;
  }
}

void CrushWrapper::decode_crush_bucket(crush_bucket** bptr, bufferlist::iterator &blp)
{
  __u32 alg;
  ::decode(alg, blp);
  if (!alg) {
    *bptr = NULL;
    return;
  }

  int size = 0;
  switch (alg) {
  case CRUSH_BUCKET_UNIFORM:
    size = sizeof(crush_bucket_uniform);
    break;
  case CRUSH_BUCKET_LIST:
    size = sizeof(crush_bucket_list);
    break;
  case CRUSH_BUCKET_TREE:
    size = sizeof(crush_bucket_tree);
    break;
  case CRUSH_BUCKET_STRAW:
    size = sizeof(crush_bucket_straw);
    break;
  default:
    {
      char str[128];
      snprintf(str, sizeof(str), "unsupported bucket algorithm: %d", alg);
      throw buffer::malformed_input(str);
    }
  }
  crush_bucket *bucket = (crush_bucket*)calloc(1, size);
  *bptr = bucket;
    
  ::decode(bucket->id, blp);
  ::decode(bucket->type, blp);
  ::decode(bucket->alg, blp);
  ::decode(bucket->hash, blp);
  ::decode(bucket->weight, blp);
  ::decode(bucket->size, blp);

  bucket->items = (__s32*)calloc(1, bucket->size * sizeof(__s32));
  for (unsigned j = 0; j < bucket->size; ++j) {
    ::decode(bucket->items[j], blp);
  }

  bucket->perm = (__u32*)calloc(1, bucket->size * sizeof(__s32));
  bucket->perm_n = 0;

  switch (bucket->alg) {
  case CRUSH_BUCKET_UNIFORM:
    ::decode(((crush_bucket_uniform*)bucket)->item_weight, blp);
    break;

  case CRUSH_BUCKET_LIST: {
    crush_bucket_list* cbl = (crush_bucket_list*)bucket;
    cbl->item_weights = (__u32*)calloc(1, bucket->size * sizeof(__u32));
    cbl->sum_weights = (__u32*)calloc(1, bucket->size * sizeof(__u32));

    for (unsigned j = 0; j < bucket->size; ++j) {
      ::decode(cbl->item_weights[j], blp);
      ::decode(cbl->sum_weights[j], blp);
    }
    break;
  }

  case CRUSH_BUCKET_TREE: {
    crush_bucket_tree* cbt = (crush_bucket_tree*)bucket;
    ::decode(cbt->num_nodes, blp);
    cbt->node_weights = (__u32*)calloc(1, cbt->num_nodes * sizeof(__u32));
    for (unsigned j=0; j<cbt->num_nodes; j++) {
      ::decode(cbt->node_weights[j], blp);
    }
    break;
  }

  case CRUSH_BUCKET_STRAW: {
    crush_bucket_straw* cbs = (crush_bucket_straw*)bucket;
    cbs->straws = (__u32*)calloc(1, bucket->size * sizeof(__u32));
    cbs->item_weights = (__u32*)calloc(1, bucket->size * sizeof(__u32));
    for (unsigned j = 0; j < bucket->size; ++j) {
      ::decode(cbs->item_weights[j], blp);
      ::decode(cbs->straws[j], blp);
    }
    break;
  }

  default:
    // We should have handled this case in the first switch statement
    assert(0);
    break;
  }
}

  
void CrushWrapper::dump(Formatter *f) const
{
  f->open_array_section("devices");
  for (int i=0; i<get_max_devices(); i++) {
    f->open_object_section("device");
    f->dump_int("id", i);
    const char *n = get_item_name(i);
    if (n) {
      f->dump_string("name", n);
    } else {
      char name[20];
      sprintf(name, "device%d", i);
      f->dump_string("name", name);
    }
    f->close_section();
  }
  f->close_section();

  f->open_array_section("types");
  int n = get_num_type_names();
  for (int i=0; n; i++) {
    const char *name = get_type_name(i);
    if (!name) {
      if (i == 0) {
	f->open_object_section("type");
	f->dump_int("type_id", 0);
	f->dump_string("name", "device");
	f->close_section();
      }
      continue;
    }
    n--;
    f->open_object_section("type");
    f->dump_int("type_id", i);
    f->dump_string("name", name);
    f->close_section();
  }
  f->close_section();

  f->open_array_section("buckets");
  for (int bucket = -1; bucket > -1-get_max_buckets(); --bucket) {
    if (!bucket_exists(bucket))
      continue;
    f->open_object_section("bucket");
    f->dump_int("id", bucket);
    if (get_item_name(bucket))
      f->dump_string("name", get_item_name(bucket));
    f->dump_int("type_id", get_bucket_type(bucket));
    if (get_type_name(get_bucket_type(bucket)))
      f->dump_string("type_name", get_type_name(get_bucket_type(bucket)));
    f->dump_int("weight", get_bucket_weight(bucket));
    f->dump_string("alg", crush_bucket_alg_name(get_bucket_alg(bucket)));
    f->dump_string("hash", crush_hash_name(get_bucket_hash(bucket)));
    f->open_array_section("items");
    for (int j=0; j<get_bucket_size(bucket); j++) {
      f->open_object_section("item");
      f->dump_int("id", get_bucket_item(bucket, j));
      f->dump_int("weight", get_bucket_item_weight(bucket, j));
      f->dump_int("pos", j);
      f->close_section();
    }
    f->close_section();
    f->close_section();
  }
  f->close_section();

  f->open_array_section("rules");
  for (int i=0; i<get_max_rules(); i++) {
    if (!rule_exists(i))
      continue;
    f->open_object_section("rule");
    f->dump_int("rule_id", i);
    if (get_rule_name(i))
      f->dump_string("rule_name", get_rule_name(i));
    f->dump_int("ruleset", get_rule_mask_ruleset(i));
    f->dump_int("type", get_rule_mask_type(i));
    f->dump_int("min_size", get_rule_mask_min_size(i));
    f->dump_int("max_size", get_rule_mask_max_size(i));
    f->open_array_section("steps");
    for (int j=0; j<get_rule_len(i); j++) {
      f->open_object_section("step");
      switch (get_rule_op(i, j)) {
      case CRUSH_RULE_NOOP:
	f->dump_string("op", "noop");
	break;
      case CRUSH_RULE_TAKE:
	f->dump_string("op", "take");
	f->dump_int("item", get_rule_arg1(i, j));
	break;
      case CRUSH_RULE_EMIT:
	f->dump_string("op", "emit");
	break;
      case CRUSH_RULE_CHOOSE_FIRSTN:
	f->dump_string("op", "choose_firstn");
	f->dump_int("num", get_rule_arg1(i, j));
	f->dump_string("type", get_type_name(get_rule_arg2(i, j)));
	break;
      case CRUSH_RULE_CHOOSE_INDEP:
	f->dump_string("op", "choose_indep");
	f->dump_int("num", get_rule_arg1(i, j));
	f->dump_string("type", get_type_name(get_rule_arg2(i, j)));
	break;
      case CRUSH_RULE_CHOOSE_LEAF_FIRSTN:
	f->dump_string("op", "chooseleaf_firstn");
	f->dump_int("num", get_rule_arg1(i, j));
	f->dump_string("type", get_type_name(get_rule_arg2(i, j)));
	break;
      case CRUSH_RULE_CHOOSE_LEAF_INDEP:
	f->dump_string("op", "chooseleaf_indep");
	f->dump_int("num", get_rule_arg1(i, j));
	f->dump_string("type", get_type_name(get_rule_arg2(i, j)));
	break;
      default:
	f->dump_int("opcode", get_rule_op(i, j));
	f->dump_int("arg1", get_rule_arg1(i, j));
	f->dump_int("arg2", get_rule_arg2(i, j));
      }
      f->close_section();
    }
    f->close_section();
    f->close_section();
  }
  f->close_section();
}

void CrushWrapper::generate_test_instances(list<CrushWrapper*>& o)
{
  o.push_back(new CrushWrapper);
  // fixme
}
