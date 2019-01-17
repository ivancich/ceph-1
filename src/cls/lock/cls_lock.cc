// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/** \file
 *
 * This is an OSD class that implements methods for object
 * advisory locking.
 *
 */

#include <errno.h>
#include <map>
#include <sstream>

#include "include/types.h"
#include "include/utime.h"
#include "objclass/objclass.h"

#include "common/errno.h"
#include "common/Clock.h"

#include "cls/lock/cls_lock_types.h"
#include "cls/lock/cls_lock_ops.h"

#include "global/global_context.h"

#include "include/compat.h"


using namespace rados::cls::lock;


CLS_VER(1,0)
CLS_NAME(lock)

#define LOCK_PREFIX    "lock."

static int clean_lock(cls_method_context_t hctx)
{
  int r = cls_cxx_remove(hctx);
  if (r < 0)
    return r;

  return 0;
}

static int read_lock(cls_method_context_t hctx,
		     const string& name,
		     lock_info_t *lock)
{
  bufferlist bl;
  string key = LOCK_PREFIX;
  key.append(name);
 
  int r = cls_cxx_getxattr(hctx, key.c_str(), &bl);
  if (r < 0) {
    if (r ==  -ENODATA) {
      *lock = lock_info_t();
      return 0;
    }
    if (r != -ENOENT) {
      CLS_ERR("error reading xattr %s: %d", key.c_str(), r);
    }
    return r;
  }

  try {
    auto it = bl.cbegin();
    decode(*lock, it);
  } catch (const buffer::error &err) {
    CLS_ERR("error decoding %s", key.c_str());
    return -EIO;
  }

  /* now trim expired locks */

  utime_t now = ceph_clock_now();

  map<locker_id_t, locker_info_t>::iterator iter = lock->lockers.begin();

  while (iter != lock->lockers.end()) {
    struct locker_info_t& info = iter->second;
    if (!info.expiration.is_zero() && info.expiration < now) {
      CLS_LOG(20, "expiring locker");
      iter = lock->lockers.erase(iter);
    } else {
      ++iter;
    }
  }

  if (lock->lockers.empty() && cls_lock_is_ephemeral(lock->lock_type)) {
    r = clean_lock(hctx);
    if (r < 0) {
      CLS_ERR("error, on read, cleaning lock object %s", cpp_strerror(r).c_str());
    }
  }

  return 0;
}

static int write_lock(cls_method_context_t hctx, const string& name, const lock_info_t& lock)
{
  using ceph::encode;
  string key = LOCK_PREFIX;
  key.append(name);

  bufferlist lock_bl;
  encode(lock, lock_bl, cls_get_client_features(hctx));

  int r = cls_cxx_setxattr(hctx, key.c_str(), &lock_bl);
  if (r < 0)
    return r;

  return 0;
}


/**
 * helper function to add a lock and update disk state.
 *
 * Input:
 * @param name Lock name
 * @param lock_type Type of lock (exclusive / shared)
 * @param duration Duration of lock (in seconds). Zero means it doesn't expire.
 *                 Also the duration of bids when a bid is submitted.
 * @param flags lock flags
 * @param cookie The cookie to set in the lock
 * @param tag The tag to match with the lock (can only lock with matching tags)
 * @param lock_description The lock description to set (if not empty)
 * @param locker_description The locker description
 * @param bidder_id Identifier of the bidder.
 * @param bid_amount Amount of the bid. -1 indicates no bid.
 *
 * @return 0 on success, or -errno on failure
 */
static int lock_obj(cls_method_context_t hctx,
                    const string& name,
                    ClsLockType lock_type,
                    const utime_t& duration,
                    const string& description,
                    uint8_t flags,
                    const string& cookie,
                    const string& tag,
		    int32_t bid_amount,
		    const utime_t& bid_duration)
{
  struct BidRecord {
    int32_t amount;
    utime_t expiration;
  };

  // three-level map, from object id, to lock name, to lock requester, to BidRecord
  using ClientBidMap = std::map<entity_name_t /* lock requester */,BidRecord>;
  using LockBidMap = std::map<std::string /* lock name */,ClientBidMap>;
  using ObjectBidMap = std::map<string /* obj id */,LockBidMap>;

  // this data does not have to persist between restarts, but it has
  // to persist between calls; therefore let's keep it in memory
  static std::mutex bids_mtx;
  static ObjectBidMap bids;
  static ObjectBidMap::iterator cleaner = bids.end();

#warning Every time through this function try to clean up two entries.

  bool exclusive = cls_lock_is_exclusive(lock_type);
  lock_info_t linfo;
  bool fail_if_exists = (flags & LOCK_FLAG_MAY_RENEW) == 0;
  bool fail_if_does_not_exist = flags & LOCK_FLAG_MUST_RENEW;

  CLS_LOG(20,
	  "requested lock_type=%s fail_if_exists=%d "
	  "fail_if_does_not_exist=%d bid_amount=%d",
	  cls_lock_type_str(lock_type),
	  fail_if_exists,
	  fail_if_does_not_exist,
	  bid_amount);
  if (!cls_lock_is_valid(lock_type)) {
    return -EINVAL;
  }

  if (name.empty())
    return -EINVAL;

  if (!fail_if_exists && fail_if_does_not_exist) {
    // at most one of LOCK_FLAG_MAY_RENEW and LOCK_FLAG_MUST_RENEW may
    // be set since they have different implications if the lock does
    // not already exist
    return -EINVAL;
  }

  utime_t now = ceph_clock_now();

  // bid related variables that we'll only assign and use if a bid is
  // present
  std::string lock_obj_id;
  entity_name_t bid_source;
  ClientBidMap* client_bid_map_ptr = nullptr;

  entity_inst_t origin;
  int r = cls_get_request_origin(hctx, &origin);
  ceph_assert(r == 0);

  // if there's a bid, update it in bids
  if (bid_amount >= 0) {
    // bidded locks must be exclusive (i.e., not be shared)
    if (!exclusive) {
      CLS_LOG(20,
	      "attempted to bid for a lock that was not exclusive "
	      "or exclusive ephemeral");
      return -EINVAL;
    }

    // collect/compute data before taking lock

    bid_source = origin.name;

    hobject_t* object_info;
    r = cls_get_obj_info(hctx, &object_info);
    ceph_assert(r == 0);

    lock_obj_id = object_info->to_str();

    CLS_LOG(20,
	    "creating bidded lock for object \"%s\", lock \"%s\", "
	    "for client \"%s\"",
	    lock_obj_id.c_str(), lock_name.c_str(), bid_source.type_str());

    // now take lock

    std::unique_lock<std::mutex> guard(bids_mtx);

    // access (or assign new) set of bid entries
    auto obj_emplace = bids.emplace(std::make_pair(lock_obj_id,LockBidMap()));
    LockBidMap& lock_bid_map =
      obj_emplace.first->second; // either pre-exiting or new

    auto lock_emplace =
      lock_bid_map.emplace(std::make_pair(lock_name, ClientBidMap()));
    client_bid_map_ptr =
      &(lock_emplace.first->second); // either pre-existing or new

    // update or insert new bid record
    BidRecord bid_rec{ bid_amount, now + bid_duration };
    auto bid_emplace =
      client_bid_map_ptr->emplace(std::make_pair(origin.name, bid_rec));
    if (!bid_emplace.second) {
      bid_emplace.first->second = bid_rec; // if didn't insert, replace
    }
  } // if (bid_amount >= 0)

  // see if there's already a locker
  r = read_lock(hctx, name, &linfo); // this also erases expired locks
  if (r < 0 && r != -ENOENT) {
    CLS_ERR("Could not read lock info: %s", cpp_strerror(r).c_str());
    return r;
  }

  map<locker_id_t, locker_info_t>& lockers = linfo.lockers;
  map<locker_id_t, locker_info_t>::iterator iter;

  locker_id_t id;
  id.cookie = cookie;
  id.locker = origin.name;

  /* check this early, before we check fail_if_exists, otherwise we might
   * remove the locker entry and not check it later */
  if (lockers.size() && tag != linfo.tag) {
    CLS_LOG(20, "cannot take lock on object, conflicting tag");
    return -EBUSY;
  }

  bool is_renewing = false;
  ClsLockType existing_lock_type = linfo.lock_type;
  CLS_LOG(20, "existing_lock_type=%s", cls_lock_type_str(existing_lock_type));
  iter = lockers.find(id);
  if (iter != lockers.end()) {
    if (fail_if_exists && !fail_if_does_not_exist) {
      // caller does not want to renew, but lock exists; since this is
      // a common situation no need to log
      return -EEXIST;
    } else {
      is_renewing = true;
      lockers.erase(iter); // remove old entry
    }
  } else if (fail_if_does_not_exist) {
      CLS_LOG(20, "there is no existing lock to renew");
    return -ENOENT;
  }

  if (!lockers.empty()) {
    if (exclusive) {
      auto locker_lister =
	[&lockers]() -> std::string {
	  std::stringstream locker_list;
	  locker_list << lockers;
	  return locker_list.str();
	};
      CLS_LOG(20, "could not exclusive-lock object, already locked by %s",
	      locker_lister().c_str());
      return -EBUSY;
    }

    if (existing_lock_type != lock_type) {
      CLS_LOG(20, "cannot take lock on object, conflicting lock type");
      return -EBUSY;
    }
  }

  // If we're renewing bid doesn't matter. But if we're not renewing
  // then we can only succeed if we have the lowest unexpired bid.
  if (!is_renewing && bid_amount >= 0) {
    std::unique_lock<std::mutex> guard(bids_mtx);
    int32_t best_bid = bid_amount; // start here but see if we can find lower
    for (auto i = client_bid_map_ptr->cbegin();
	 i != client_bid_map_ptr->cend();
	 /* empty */) {
      if (i->second.expiration < now) {
	// "< now" allows a bid with duration of 0 to count;
	// essentially that's a one-off bid
	i = client_bid_map_ptr->erase(i); // update iterator and go on
	continue;
      } else if (i->second.amount < best_bid) {
	best_bid = i->second.amount;
      }
      ++i;
    }

    if (bid_amount != best_bid) {
      // lock fails if this bid wasn't among the lowest unexpired bids
      CLS_LOG(20, "could not lock object due to lower bid");
      return -EBUSY;
    }
  }

  linfo.lock_type = lock_type;
  linfo.tag = tag;
  utime_t expiration;
  if (!duration.is_zero()) {
    expiration = now + duration;
  }

  // make all addrs of type legacy, because v2 clients speak v2 or v1,
  // even depending on which OSD they are talking to, and the type
  // isn't what uniquely identifies them.  also, storing a v1 addr
  // here means that old clients who get this locker_info won't see an
  // old "msgr2:" prefix.
  origin.addr.set_type(entity_addr_t::TYPE_LEGACY);

  struct locker_info_t info(expiration, origin.addr, description);

  linfo.lockers[id] = info;

  r = write_lock(hctx, name, linfo);
  if (r < 0)
    return r;

  return 0;
} // lock_obj

/**
 * Set an exclusive lock on an object for the activating client, if possible.
 *
 * Input:
 * @param cls_lock_lock_op request input
 *
 * @returns 0 on success, -EINVAL if it can't decode the lock_cookie,
 * -EBUSY if the object is already locked, or -errno on (unexpected) failure.
 */
static int lock_op(cls_method_context_t hctx,
                   bufferlist *in, bufferlist *out)
{
  CLS_LOG(20, "lock_op");
  cls_lock_lock_op op;
  try {
    auto iter = in->cbegin();
    decode(op, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  return lock_obj(hctx,
                  op.name, op.type, op.duration, op.description,
                  op.flags, op.cookie, op.tag,
		  op.bid_amount, op.bid_duration);
}

/**
 *  helper function to remove a lock from on disk and clean up state.
 *
 *  @param name The lock name
 *  @param locker The locker entity name
 *  @param cookie The user-defined cookie associated with the lock.
 *
 *  @return 0 on success, -ENOENT if there is no such lock (either
 *  entity or cookie is wrong), or -errno on other error.
 */
static int remove_lock(cls_method_context_t hctx,
		       const string& name,
		       entity_name_t& locker,
		       const string& cookie)
{
  // get current lockers
  lock_info_t linfo;
  int r = read_lock(hctx, name, &linfo);
  if (r < 0) {
    CLS_ERR("Could not read list of current lockers off disk: %s", cpp_strerror(r).c_str());
    return r;
  }

  map<locker_id_t, locker_info_t>& lockers = linfo.lockers;
  struct locker_id_t id(locker, cookie);

  // remove named locker from set
  map<locker_id_t, locker_info_t>::iterator iter = lockers.find(id);
  if (iter == lockers.end()) { // no such key
    return -ENOENT;
  }
  lockers.erase(iter);

  if (cls_lock_is_ephemeral(linfo.lock_type)) {
    ceph_assert(lockers.empty());
    r = clean_lock(hctx);
  } else {
    r = write_lock(hctx, name, linfo);
  }

  return r;
}

/**
 * Unlock an object which the activating client currently has locked.
 *
 * Input:
 * @param cls_lock_unlock_op request input
 *
 * @return 0 on success, -EINVAL if it can't decode the cookie, -ENOENT
 * if there is no such lock (either entity or cookie is wrong), or
 * -errno on other (unexpected) error.
 */
static int unlock_op(cls_method_context_t hctx,
                     bufferlist *in, bufferlist *out)
{
  CLS_LOG(20, "unlock_op");
  cls_lock_unlock_op op;
  try {
    auto iter = in->cbegin();
    decode(op, iter);
  } catch (const buffer::error& err) {
    return -EINVAL;
  }

  entity_inst_t inst;
  int r = cls_get_request_origin(hctx, &inst);
  ceph_assert(r == 0);
  return remove_lock(hctx, op.name, inst.name, op.cookie);
}

/**
 * Break the lock on an object held by any client.
 *
 * Input:
 * @param cls_lock_break_op request input
 *
 * @return 0 on success, -EINVAL if it can't decode the locker and
 * cookie, -ENOENT if there is no such lock (either entity or cookie
 * is wrong), or -errno on other (unexpected) error.
 */
static int break_lock(cls_method_context_t hctx,
		      bufferlist *in, bufferlist *out)
{
  CLS_LOG(20, "break_lock");
  cls_lock_break_op op;
  try {
    auto iter = in->cbegin();
    decode(op, iter);
  } catch (const buffer::error& err) {
    return -EINVAL;
  }

  return remove_lock(hctx, op.name, op.locker, op.cookie);
}


/**
 * Retrieve lock info: lockers, tag, exclusive
 *
 * Input:
 * @param cls_lock_list_lockers_op request input
 *
 * Output:
 * @param cls_lock_list_lockers_reply result
 *
 * @return 0 on success, -errno on failure.
 */
static int get_info(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  CLS_LOG(20, "get_info");
  cls_lock_get_info_op op;
  try {
    auto iter = in->cbegin();
    decode(op, iter);
  } catch (const buffer::error& err) {
    return -EINVAL;
  }

  // get current lockers
  lock_info_t linfo;
  int r = read_lock(hctx, op.name, &linfo);
  if (r < 0) {
    CLS_ERR("Could not read lock info: %s", cpp_strerror(r).c_str());
    return r;
  }

  struct cls_lock_get_info_reply ret;

  map<locker_id_t, locker_info_t>::iterator iter;
  for (iter = linfo.lockers.begin(); iter != linfo.lockers.end(); ++iter) {
    ret.lockers[iter->first] = iter->second;
  }
  ret.lock_type = linfo.lock_type;
  ret.tag = linfo.tag;

  encode(ret, *out, cls_get_client_features(hctx));

  return 0;
}


/**
 * Retrieve a list of locks for this object
 *
 * Input:
 * @param in is ignored.
 *
 * Output:
 * @param out contains encoded cls_list_locks_reply
 *
 * @return 0 on success, -errno on failure.
 */
static int list_locks(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  CLS_LOG(20, "list_locks");

  map<string, bufferlist> attrs;

  int r = cls_cxx_getxattrs(hctx, &attrs);
  if (r < 0)
    return r;

  cls_lock_list_locks_reply ret;

  map<string, bufferlist>::iterator iter;
  size_t pos = sizeof(LOCK_PREFIX) - 1;
  for (iter = attrs.begin(); iter != attrs.end(); ++iter) {
    const string& attr = iter->first;
    if (attr.substr(0, pos).compare(LOCK_PREFIX) == 0) {
      ret.locks.push_back(attr.substr(pos));
    }
  }

  encode(ret, *out);

  return 0;
}

/**
 * Assert that the object is currently locked
 *
 * Input:
 * @param cls_lock_assert_op request input
 *
 * Output:
 * @param none
 *
 * @return 0 on success, -errno on failure.
 */
int assert_locked(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  CLS_LOG(20, "assert_locked");

  cls_lock_assert_op op;
  try {
    auto iter = in->cbegin();
    decode(op, iter);
  } catch (const buffer::error& err) {
    return -EINVAL;
  }

  if (!cls_lock_is_valid(op.type)) {
    return -EINVAL;
  }

  if (op.name.empty()) {
    return -EINVAL;
  }

  // see if there's already a locker
  lock_info_t linfo;
  int r = read_lock(hctx, op.name, &linfo);
  if (r < 0) {
    CLS_ERR("Could not read lock info: %s", cpp_strerror(r).c_str());
    return r;
  }

  if (linfo.lockers.empty()) {
    CLS_LOG(20, "object not locked");
    return -EBUSY;
  }

  if (linfo.lock_type != op.type) {
    CLS_LOG(20, "lock type mismatch: current=%s, assert=%s",
            cls_lock_type_str(linfo.lock_type), cls_lock_type_str(op.type));
    return -EBUSY;
  }

  if (linfo.tag != op.tag) {
    CLS_LOG(20, "lock tag mismatch: current=%s, assert=%s", linfo.tag.c_str(),
            op.tag.c_str());
    return -EBUSY;
  }

  entity_inst_t inst;
  r = cls_get_request_origin(hctx, &inst);
  ceph_assert(r == 0);

  locker_id_t id;
  id.cookie = op.cookie;
  id.locker = inst.name;

  map<locker_id_t, locker_info_t>::iterator iter = linfo.lockers.find(id);
  if (iter == linfo.lockers.end()) {
    CLS_LOG(20, "not locked by assert client");
    return -EBUSY;
  }
  return 0;
}

/**
 * Update the cookie associated with an object lock
 *
 * Input:
 * @param cls_lock_set_cookie_op request input
 *
 * Output:
 * @param none
 *
 * @return 0 on success, -errno on failure.
 */
int set_cookie(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  CLS_LOG(20, "set_cookie");

  cls_lock_set_cookie_op op;
  try {
    auto iter = in->cbegin();
    decode(op, iter);
  } catch (const buffer::error& err) {
    return -EINVAL;
  }

  if (!cls_lock_is_valid(op.type)) {
    return -EINVAL;
  }

  if (op.name.empty()) {
    return -EINVAL;
  }

  // see if there's already a locker
  lock_info_t linfo;
  int r = read_lock(hctx, op.name, &linfo);
  if (r < 0) {
    CLS_ERR("Could not read lock info: %s", cpp_strerror(r).c_str());
    return r;
  }

  if (linfo.lockers.empty()) {
    CLS_LOG(20, "object not locked");
    return -EBUSY;
  }

  if (linfo.lock_type != op.type) {
    CLS_LOG(20, "lock type mismatch: current=%s, assert=%s",
            cls_lock_type_str(linfo.lock_type), cls_lock_type_str(op.type));
    return -EBUSY;
  }

  if (linfo.tag != op.tag) {
    CLS_LOG(20, "lock tag mismatch: current=%s, assert=%s", linfo.tag.c_str(),
            op.tag.c_str());
    return -EBUSY;
  }

  entity_inst_t inst;
  r = cls_get_request_origin(hctx, &inst);
  ceph_assert(r == 0);

  locker_id_t id;
  id.cookie = op.cookie;
  id.locker = inst.name;

  map<locker_id_t, locker_info_t>::iterator iter = linfo.lockers.find(id);
  if (iter == linfo.lockers.end()) {
    CLS_LOG(20, "not locked by client");
    return -EBUSY;
  }

  id.cookie = op.new_cookie;
  if (linfo.lockers.count(id) != 0) {
    CLS_LOG(20, "lock cookie in-use");
    return -EBUSY;
  }

  locker_info_t locker_info(iter->second);
  linfo.lockers.erase(iter);

  linfo.lockers[id] = locker_info;
  r = write_lock(hctx, op.name, linfo);
  if (r < 0) {
    CLS_ERR("Could not update lock info: %s", cpp_strerror(r).c_str());
    return r;
  }
  return 0;
}

CLS_INIT(lock)
{
  CLS_LOG(20, "Loaded lock class!");

  cls_handle_t h_class;
  cls_method_handle_t h_lock_op;
  cls_method_handle_t h_unlock_op;
  cls_method_handle_t h_break_lock;
  cls_method_handle_t h_get_info;
  cls_method_handle_t h_list_locks;
  cls_method_handle_t h_assert_locked;
  cls_method_handle_t h_set_cookie;

  cls_register("lock", &h_class);
  cls_register_cxx_method(h_class, "lock",
                          CLS_METHOD_RD | CLS_METHOD_WR | CLS_METHOD_PROMOTE,
                          lock_op, &h_lock_op);
  cls_register_cxx_method(h_class, "unlock",
                          CLS_METHOD_RD | CLS_METHOD_WR | CLS_METHOD_PROMOTE,
                          unlock_op, &h_unlock_op);
  cls_register_cxx_method(h_class, "break_lock",
                          CLS_METHOD_RD | CLS_METHOD_WR,
                          break_lock, &h_break_lock);
  cls_register_cxx_method(h_class, "get_info",
                          CLS_METHOD_RD,
                          get_info, &h_get_info);
  cls_register_cxx_method(h_class, "list_locks",
                          CLS_METHOD_RD,
                          list_locks, &h_list_locks);
  cls_register_cxx_method(h_class, "assert_locked",
                          CLS_METHOD_RD | CLS_METHOD_PROMOTE,
                          assert_locked, &h_assert_locked);
  cls_register_cxx_method(h_class, "set_cookie",
                          CLS_METHOD_RD | CLS_METHOD_WR | CLS_METHOD_PROMOTE,
                          set_cookie, &h_set_cookie);

  return;
}
