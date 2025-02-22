/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifndef OCEANBASE_MEMTABLE_MVCC_OB_MVCC_READ_CTX_
#define OCEANBASE_MEMTABLE_MVCC_OB_MVCC_READ_CTX_

#include "share/ob_define.h"
#include "storage/tx/ob_trans_define.h"
#include "lib/oblog/ob_log.h"
#include "lib/oblog/ob_log_module.h"

namespace oceanbase
{
namespace transaction {
class ObPartTransCtx;
}

namespace storage {
class ObTxTable;
class ObTxTableGuard;
}

namespace memtable
{
class ObQueryAllocator;
class ObMemtableCtx;

class ObMvccAccessCtx
{
public:
  ObMvccAccessCtx()
    : type_(T::INVL),
      abs_lock_timeout_(-1),
      tx_lock_timeout_(-1),
      snapshot_(),
      tx_table_guard_(),
      tx_id_(),
      tx_desc_(NULL),
      tx_ctx_(NULL),
      mem_ctx_(NULL),
      tx_scn_(-1),
      handle_start_time_(OB_INVALID_TIMESTAMP)
  {}
  ~ObMvccAccessCtx() {
    type_ = T::INVL;
    abs_lock_timeout_ = -1;
    tx_lock_timeout_ = -1;
    tx_id_.reset();
    tx_desc_ = NULL;
    tx_ctx_ = NULL;
    mem_ctx_ = NULL;
    tx_scn_ = -1;
    handle_start_time_ = OB_INVALID_TIMESTAMP;
  }
  void reset() {
    if (is_write() && OB_UNLIKELY(tx_ctx_)) {
      warn_tx_ctx_leaky_();
    }
    type_ = T::INVL;
    abs_lock_timeout_ = -1;
    tx_lock_timeout_ = -1;
    snapshot_.reset();
    tx_table_guard_.reset();
    tx_id_.reset();
    tx_desc_ = NULL;
    tx_ctx_ = NULL;
    mem_ctx_ = NULL;
    tx_scn_ = -1;
    handle_start_time_ = OB_INVALID_TIMESTAMP;
  }
  bool is_valid() const {
    switch(type_) {
    case T::STRONG_READ: return is_read_valid__();
    case T::WEAK_READ: return is_read_valid__();
    case T::WRITE: return is_write_valid__();
    case T::REPLAY: return is_replay_valid__();
    default: return false;
    }
  }
  bool is_write_valid__() const {
    return abs_lock_timeout_ >= 0
      && snapshot_.is_valid()
      && tx_ctx_
      && mem_ctx_
      && tx_scn_ > 0
      && tx_id_.is_valid()
      && tx_table_guard_.is_valid();
  }
  bool is_replay_valid__() const {
    return tx_ctx_
      && mem_ctx_
      && tx_id_.is_valid();
  }
  bool is_read_valid__() const {
    return abs_lock_timeout_ >= 0
      && snapshot_.is_valid()
      && tx_table_guard_.is_valid()
      && (!tx_ctx_ || mem_ctx_);
  }
  void init_read(transaction::ObPartTransCtx *tx_ctx, /* nullable */
                 ObMemtableCtx *mem_ctx, /* nullable */
                 const storage::ObTxTableGuard &tx_table_guard,
                 const transaction::ObTxSnapshot &snapshot,
                 const int64_t abs_lock_timeout,
                 const int64_t tx_lock_timeout,
                 const bool is_weak_read)
  {
    reset();
    type_ = is_weak_read ? T::WEAK_READ : T::STRONG_READ;
    tx_ctx_ = tx_ctx;
    mem_ctx_ = mem_ctx;
    tx_table_guard_ = tx_table_guard;
    snapshot_ = snapshot;
    abs_lock_timeout_ = abs_lock_timeout;
    tx_lock_timeout_ = tx_lock_timeout;
  }
  // light read, used by storage background merge/compaction routine
  void init_read(const storage::ObTxTableGuard &tx_table_guard,
                 const share::SCN snapshot_version,
                 const int64_t timeout,
                 const int64_t tx_lock_timeout)
  {
    transaction::ObTxSnapshot snapshot;
    snapshot.version_ = snapshot_version;
    init_read(NULL, NULL, tx_table_guard, snapshot, timeout, tx_lock_timeout, false);
  }
  void init_write(transaction::ObPartTransCtx &tx_ctx,
                  ObMemtableCtx &mem_ctx,
                  const transaction::ObTransID &tx_id,
                  const int64_t tx_scn,
                  transaction::ObTxDesc &tx_desc,
                  const storage::ObTxTableGuard &tx_table_guard,
                  const transaction::ObTxSnapshot &snapshot,
                  const int64_t abs_lock_timeout,
                  const int64_t tx_lock_timeout)
  {
    reset();
    type_ = T::WRITE;
    tx_ctx_ = &tx_ctx;
    mem_ctx_ = &mem_ctx;
    tx_id_ = tx_id;
    tx_scn_ = tx_scn;
    tx_desc_ = &tx_desc;
    tx_table_guard_ = tx_table_guard;
    snapshot_ = snapshot;
    abs_lock_timeout_ = abs_lock_timeout;
    tx_lock_timeout_ = tx_lock_timeout;
  }
  void init_replay(transaction::ObPartTransCtx &tx_ctx,
                   ObMemtableCtx &mem_ctx,
                   const transaction::ObTransID &tx_id)
  {
    reset();
    type_ = T::REPLAY;
    tx_ctx_ = &tx_ctx;
    mem_ctx_ = &mem_ctx;
    tx_id_ = tx_id;
  }
  const transaction::ObTransID &get_tx_id() const {
    return tx_id_;
  }
  share::SCN get_snapshot_version() const {
    return snapshot_.version_;
  }
  storage::ObTxTable *get_tx_table() const {
    return tx_table_guard_.get_tx_table();
  }
  const storage::ObTxTableGuard &get_tx_table_guard() const {
    return tx_table_guard_;
  }
  ObMemtableCtx *get_mem_ctx() const {
    return mem_ctx_;
  }
  bool is_read() const { return type_ == T::STRONG_READ || type_ == T::WEAK_READ; }
  bool is_weak_read() const { return type_ == T::WEAK_READ; }
  bool is_write() const { return type_ == T::WRITE; }
  bool is_replay() const { return type_ == T::REPLAY; }
  int64_t eval_lock_expire_ts(int64_t lock_wait_start_ts = 0) const {
    int64_t expire_ts = OB_INVALID_TIMESTAMP;
    if (tx_lock_timeout_ >= 0) {
      // Case 1: When tx_lock_timeout is bigger than 0, we use the minimum of
      //         the tx_lock_timeout plus remaining time(defined from system
      //         variable) and abs_lock_timeout(calcualted from select-for-update
      //         timeout).
      // Case 2: When tx_lock_timeout is euqal to 0, we use the remaining time
      //         as timeout(And it must trigger timeout when write-write conflict)
      lock_wait_start_ts = lock_wait_start_ts > 0 ?
        lock_wait_start_ts : ObTimeUtility::current_time();
      expire_ts = MIN(lock_wait_start_ts + tx_lock_timeout_, abs_lock_timeout_);
    } else {
      // Case 2: When tx_lock_timeout is smaller than 0, we use abs_lock_timeout
      //         as timeout(calcualted from select-for-update timeout).
      expire_ts = abs_lock_timeout_;
    }
    return expire_ts;
  }
  TO_STRING_KV(K_(type),
               K_(abs_lock_timeout),
               K_(tx_lock_timeout),
               K_(snapshot),
               K_(tx_table_guard),
               K_(tx_id),
               KPC_(tx_desc),
               KP_(tx_ctx),
               KP_(mem_ctx),
               K_(tx_scn));
private:
  void warn_tx_ctx_leaky_();
public: // NOTE: those field should only be accessed by txn relative routine
  enum class T { INVL, STRONG_READ, WEAK_READ, WRITE, REPLAY } type_;
  // abs_lock_timeout is calculated from the minimum of the wait time of the
  // select_for_update and timeout in dml_param / scan_param
  int64_t abs_lock_timeout_;
  // tx_lock_timeout is defined as a system variable `ob_trx_lock_timeout`,
  // as the timeout of waiting on the WW conflict. it timeout reached
  // return OB_ERR_EXCLUSIVE_LOCK_CONFLICT error to SQL
  // SQL will stop retry, otherwise return OB_TRY_LOCK_ROW_CONFLICT, SQL will
  // retry until timeout
  // - When ob_trx_lock_timeout is smaller than 0, the timeout is equal to
  //   ob_query_timeout
  // - When ob_trx_lock_timeout is bigger than 0, the timeout is equal to the
  //   minimum between ob_query_timeout and ob_trx_lock_timeout
  // - When ob_trx_lock_timeout is equal to 0, it means never wait
  int64_t tx_lock_timeout_;
  transaction::ObTxSnapshot snapshot_;
  storage::ObTxTableGuard tx_table_guard_;
  // specials for MvccWrite
  transaction::ObTransID tx_id_;
  transaction::ObTxDesc *tx_desc_;      // the txn descriptor
  transaction::ObPartTransCtx *tx_ctx_; // the txn context
  ObMemtableCtx *mem_ctx_;              // memtable-ctx
  int64_t tx_scn_;                      // the change's number of this modify

  // this was used for runtime mertic
  int64_t handle_start_time_;
};
} // memtable
} // oceanbase

#endif //OCEANBASE_MEMTABLE_MVCC_OB_MVCC_READ_CTX_
