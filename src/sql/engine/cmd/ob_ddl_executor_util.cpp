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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_ddl_executor_util.h"
#include "lib/utility/utility.h"
#include "lib/utility/ob_tracepoint.h"
#include "lib/worker.h"
#include "share/ob_common_rpc_proxy.h"
#include "share/ob_srv_rpc_proxy.h"      //ObSrvRpcProxy
#include "share/ob_ddl_error_message_table_operator.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;
using namespace observer;
namespace sql
{
int ObDDLExecutorUtil::handle_session_exception(ObSQLSessionInfo &session)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(session.is_query_killed())) {
    ret = OB_ERR_QUERY_INTERRUPTED;
    LOG_WARN("query is killed", K(ret));
  } else if (OB_UNLIKELY(session.is_zombie())) {
    ret = OB_SESSION_KILLED;
    LOG_WARN("session is killed", K(ret));
  } else if (GCTX.is_standby_cluster()) {
    ret = OB_SESSION_KILLED;
    LOG_INFO("cluster switchoverd, kill session", KR(ret));
  }
  return ret;
}

int ObDDLExecutorUtil::wait_ddl_finish(
    const uint64_t tenant_id,
    const int64_t task_id,
    ObSQLSessionInfo &session,
    obrpc::ObCommonRpcProxy *common_rpc_proxy,
    const bool is_support_cancel)
{
  int ret = OB_SUCCESS;
  const int64_t retry_interval = 100 * 1000;
  ObAddr unused_addr;
  bool is_table_exist = false;
  int64_t unused_user_msg_len = 0;
  THIS_WORKER.set_timeout_ts(ObTimeUtility::current_time() + OB_MAX_USER_SPECIFIED_TIMEOUT);
  ObDDLErrorMessageTableOperator::ObBuildDDLErrorMessage error_message;
  if (OB_UNLIKELY(OB_INVALID_ID == tenant_id || task_id <= 0 || nullptr == common_rpc_proxy)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tenant_id), K(task_id), KP(common_rpc_proxy));
  } else {
    int tmp_ret = OB_SUCCESS;
    bool is_tenant_dropped = false;
    while (OB_SUCC(ret)) {
      if (OB_SUCCESS == ObDDLErrorMessageTableOperator::get_ddl_error_message(
          tenant_id, task_id, -1 /* target_object_id */, unused_addr, false /* is_ddl_retry_task */, *GCTX.sql_proxy_, error_message, unused_user_msg_len)) {
        ret = error_message.ret_code_;
        if (OB_SUCCESS != ret) {
          FORWARD_USER_ERROR(ret, error_message.user_message_);
        }
        break;
      } else if (OB_TMP_FAIL(GSCHEMASERVICE.check_if_tenant_has_been_dropped(
                               tenant_id, is_tenant_dropped))) {
        LOG_WARN("check if tenant has been dropped failed", K(tmp_ret), K(tenant_id));
      } else if (is_tenant_dropped) {
        ret = OB_TENANT_HAS_BEEN_DROPPED;
        LOG_WARN("tenant has been dropped", K(ret), K(tenant_id));
        break;
      } else if (OB_FAIL(handle_session_exception(session))) {
        LOG_WARN("session exeception happened", K(ret), K(is_support_cancel));
        if (is_support_cancel && OB_TMP_FAIL(cancel_ddl_task(tenant_id, common_rpc_proxy))) {
          LOG_WARN("cancel ddl task failed", K(tmp_ret));
          ret = OB_SUCCESS;
        } else {
          break;
        }
      } else {
        ob_usleep(retry_interval);
      }
    }
  }
  return ret;
}

int ObDDLExecutorUtil::wait_build_index_finish(const uint64_t tenant_id, const int64_t task_id, bool &is_finish)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  bool is_tenant_dropped = false;
  ObAddr unused_addr;
  int64_t unused_user_msg_len = 0;
  THIS_WORKER.set_timeout_ts(ObTimeUtility::current_time() + OB_MAX_USER_SPECIFIED_TIMEOUT);
  share::ObDDLErrorMessageTableOperator::ObBuildDDLErrorMessage error_message;
  is_finish = false;
  LOG_INFO("wait build index finish", K(table_id), K(task_id));
  if (OB_UNLIKELY(OB_INVALID_ID == tenant_id || task_id <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(tenant_id), K(task_id));
  } else if (OB_SUCCESS == share::ObDDLErrorMessageTableOperator::get_ddl_error_message(
      tenant_id, task_id, -1 /* target_object_id */, unused_addr, false /* is_ddl_retry_task */, *GCTX.sql_proxy_, error_message, unused_user_msg_len)) {
    ret = error_message.ret_code_;
    if (OB_SUCCESS != ret) {
      FORWARD_USER_ERROR(ret, error_message.user_message_);
    }
    is_finish = true;
  } else if (OB_TMP_FAIL(GSCHEMASERVICE.check_if_tenant_has_been_dropped(
                 tenant_id, is_tenant_dropped))) {
    LOG_WARN("check if tenant has been dropped failed", K(tmp_ret), K(tenant_id));
  } else if (is_tenant_dropped) {
    ret = OB_TENANT_HAS_BEEN_DROPPED;
    LOG_WARN("tenant has been dropped", K(ret), K(tenant_id));
  }
  return ret;
}

int ObDDLExecutorUtil::wait_ddl_retry_task_finish(
    const uint64_t tenant_id,
    const int64_t task_id,
    ObSQLSessionInfo &session,
    obrpc::ObCommonRpcProxy *common_rpc_proxy,
    int64_t &affected_rows)
{
  int ret = OB_SUCCESS;
  affected_rows = 0;
  const int64_t retry_interval = 100 * 1000;
  ObAddr unused_addr;
  bool is_table_exist = false;
  int64_t forward_user_msg_len = 0;
  THIS_WORKER.set_timeout_ts(ObTimeUtility::current_time() + OB_MAX_USER_SPECIFIED_TIMEOUT);
  ObDDLErrorMessageTableOperator::ObBuildDDLErrorMessage error_message;
  if (OB_UNLIKELY(OB_INVALID_ID == tenant_id || task_id <= 0 || nullptr == common_rpc_proxy)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tenant_id), K(task_id), KP(common_rpc_proxy));
  } else {
    bool is_tenant_dropped = false;
    int tmp_ret = OB_SUCCESS;
    while (OB_SUCC(ret)) {
      if (OB_SUCCESS == ObDDLErrorMessageTableOperator::get_ddl_error_message(
          tenant_id, task_id, -1 /* target_object_id */, unused_addr, true /* is_ddl_retry_task */, *GCTX.sql_proxy_, error_message, forward_user_msg_len)) {
        // Here, `forward_user_msg_len` is the length of serialized hex user message.
        // Forward_user_msg_len is not 0, which means ObRpcResultCode is not empty. Thus, we need to
        // forward_user_error/ forward_user_warn/ forward_user_note.
        ret = error_message.ret_code_;
        if (OB_UNLIKELY(forward_user_msg_len == 0 && OB_SUCCESS != error_message.ret_code_)) {
          const char *str_user_error = ob_errpkt_strerror(error_message.ret_code_, lib::is_oracle_mode());
          FORWARD_USER_ERROR(error_message.ret_code_, str_user_error);
          FLOG_INFO("error code is not succ, but forward user msg is null", K(ret), K(error_message), K(str_user_error));
        } else if (forward_user_msg_len > 0) {
          int64_t pos = 0;
          int tmp_ret = OB_SUCCESS;
          obrpc::ObRpcResultCode result_code;
          if (OB_SUCCESS != (tmp_ret = result_code.deserialize(error_message.user_message_, forward_user_msg_len, pos))) {
            LOG_WARN("deserialize rpc result code failed", K(ret), K(tmp_ret), K(forward_user_msg_len), K(error_message));
          } else if (OB_UNLIKELY(OB_SUCCESS != result_code.rcode_)) {
            FORWARD_USER_ERROR(result_code.rcode_, result_code.msg_);
          } else if (lib::is_oracle_mode()) {
          } else {
            for (int i = 0; OB_SUCCESS == tmp_ret && i < result_code.warnings_.count(); ++i) {
              const common::ObWarningBuffer::WarningItem warning_item = result_code.warnings_.at(i);
              if (ObLogger::USER_WARN == warning_item.log_level_) {
                FORWARD_USER_WARN(warning_item.code_, warning_item.msg_);
              } else if (ObLogger::USER_NOTE == warning_item.log_level_) {
                FORWARD_USER_NOTE(warning_item.code_, warning_item.msg_);
              } else {
                tmp_ret = OB_ERR_UNEXPECTED;
                LOG_WARN("unknown log type", K(ret), K(tmp_ret), K(warning_item));
              }
            }
          }
        }
        break;
      } else if (OB_TMP_FAIL(GSCHEMASERVICE.check_if_tenant_has_been_dropped(
                    tenant_id, is_tenant_dropped))) {
        LOG_WARN("check if tenant has been dropped failed", K(tmp_ret), K(tenant_id));
      } else if (is_tenant_dropped) {
        ret = OB_TENANT_HAS_BEEN_DROPPED;
        LOG_WARN("tenant has been dropped", K(ret), K(tenant_id));
        break;
      } else if (OB_FAIL(handle_session_exception(session))) {
        LOG_WARN("session exeception happened", K(ret));
        if (OB_TMP_FAIL(cancel_ddl_task(tenant_id, common_rpc_proxy))) {
          LOG_WARN("cancel ddl task failed", K(tmp_ret));
          ret = OB_SUCCESS;
        } else {
          break;
        }
      } else {
        ob_usleep(retry_interval);
      }
    }
    affected_rows = error_message.affected_rows_;
  }
  return ret;
}

int ObDDLExecutorUtil::cancel_ddl_task(const int64_t tenant_id, obrpc::ObCommonRpcProxy *common_rpc_proxy)
{
  int ret = OB_SUCCESS;
  obrpc::ObCancelTaskArg rpc_arg;
  rpc_arg.task_id_ = *ObCurTraceId::get_trace_id();
  if (OB_FAIL(GCTX.srv_rpc_proxy_->to(common_rpc_proxy->get_server()).cancel_sys_task(rpc_arg))) {
    LOG_WARN("failed to cancel remote sys task", K(ret), K(rpc_arg));
  } else {
    LOG_INFO("succeed to cancel sys task", K(rpc_arg));
  }
  return ret;
}

} //end namespace sql
} //end namespace oceanbase
