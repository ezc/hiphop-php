/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include <runtime/base/server/xbox_server.h>
#include <runtime/base/runtime_option.h>
#include <runtime/base/server/rpc_request_handler.h>
#include <runtime/base/server/satellite_server.h>
#include <runtime/base/util/libevent_http_client.h>
#include <runtime/base/server/job_queue_vm_stack.h>
#include <runtime/ext/ext_json.h>
#include <util/job_queue.h>
#include <util/lock.h>
#include <util/logger.h>
#include <system/lib/systemlib.h>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

class XboxTransport : public Transport, public Synchronizable {
public:
  XboxTransport(CStrRef message, CStrRef reqInitDoc = "")
      : m_refCount(0), m_done(false), m_code(0) {
    gettime(CLOCK_MONOTONIC, &m_queueTime);

    m_message.append(message.data(), message.size());
    m_reqInitDoc.append(reqInitDoc.data(), reqInitDoc.size());
    disableCompression(); // so we don't have to decompress during sendImpl()
  }

  timespec getStartTimer() const { return m_queueTime; }

  /**
   * Implementing Transport...
   */
  virtual const char *getUrl() {
    if (!m_reqInitDoc.empty()) {
      return "xbox_process_call_message";
    }
    return RuntimeOption::XboxProcessMessageFunc.c_str();
  }
  virtual const char *getRemoteHost() {
    return "127.0.0.1";
  }
  virtual uint16 getRemotePort() {
    return 0;
  }
  virtual const void *getPostData(int &size) {
    size = m_message.size();
    return m_message.data();
  }
  virtual Method getMethod() {
    return Transport::POST;
  }
  virtual std::string getHeader(const char *name) {
    if (!strcasecmp(name, "Host")) return m_host;
    if (!strcasecmp(name, "ReqInitDoc")) return m_reqInitDoc;
    return "";
  }
  virtual void getHeaders(HeaderMap &headers) {
    // do nothing
  }
  virtual void addHeaderImpl(const char *name, const char *value) {
    // do nothing
  }
  virtual void removeHeaderImpl(const char *name) {
    // do nothing
  }
  virtual void sendImpl(const void *data, int size, int code,
                        bool chunked) {
    m_response.append((const char*)data, size);
    if (code) {
      m_code = code;
    }
  }
  virtual void onSendEndImpl() {
    Lock lock(this);
    m_done = true;
    notify();
  }

  // task interface
  bool isDone() {
    return m_done;
  }

  String getResults(int &code, int timeout_ms = 0) {
    {
      Lock lock(this);
      while (!m_done) {
        if (timeout_ms > 0) {
          long long seconds = timeout_ms / 1000;
          long long nanosecs = (timeout_ms - seconds * 1000) * 1000;
          if (!wait(seconds, nanosecs)) {
            code = -1;
            return "";
          }
        } else {
          wait();
        }
      }
    }

    String response(m_response.c_str(), m_response.size(), CopyString);
    code = m_code;
    return response;
  }

  // ref counting
  void incRefCount() {
    atomic_inc(m_refCount);
  }
  void decRefCount() {
    assert(m_refCount);
    if (atomic_dec(m_refCount) == 0) {
      delete this;
    }
  }

  void setHost(const std::string &host) { m_host = host;}
private:
  int m_refCount;

  string m_message;

  bool m_done;
  string m_response;
  int m_code;
  string m_host;
  string m_reqInitDoc;
};

class XboxRequestHandler: public RPCRequestHandler {
public:
  XboxRequestHandler() : RPCRequestHandler(Info) {}
  static bool Info;
};

bool XboxRequestHandler::Info = false;

///////////////////////////////////////////////////////////////////////////////

static IMPLEMENT_THREAD_LOCAL(XboxServerInfoPtr, s_xbox_server_info);
static IMPLEMENT_THREAD_LOCAL(XboxRequestHandler, s_xbox_request_handler);
static IMPLEMENT_THREAD_LOCAL(string, s_xbox_prev_req_init_doc);
///////////////////////////////////////////////////////////////////////////////

struct XboxWorker
  : JobQueueWorker<XboxTransport*,true,false,JobQueueDropVMStack>
{
  virtual void doJob(XboxTransport *job) {
    try {
      // If this job or the previous job that ran on this thread have
      // a custom initial document, make sure we do a reset
      string reqInitDoc = job->getHeader("ReqInitDoc");
      bool needReset = !reqInitDoc.empty() ||
                       !s_xbox_prev_req_init_doc->empty();
      *s_xbox_prev_req_init_doc = reqInitDoc;

      job->onRequestStart(job->getStartTimer());
      createRequestHandler(needReset)->handleRequest(job);
      job->decRefCount();
    } catch (...) {
      Logger::Error("RpcRequestHandler leaked exceptions");
    }
  }
private:
  RequestHandler *createRequestHandler(bool needReset = false) {
    if (!*s_xbox_server_info) {
      *s_xbox_server_info = XboxServerInfoPtr(new XboxServerInfo());
    }
    if (RuntimeOption::XboxServerLogInfo) XboxRequestHandler::Info = true;
    s_xbox_request_handler->setServerInfo(*s_xbox_server_info);
    s_xbox_request_handler->setReturnEncodeType(RPCRequestHandler::Serialize);
    if (needReset ||
        s_xbox_request_handler->needReset() ||
        s_xbox_request_handler->incRequest() >
        (*s_xbox_server_info)->getMaxRequest()) {
      Logger::Verbose("resetting xbox request handler");
      s_xbox_request_handler.destroy();
      s_xbox_request_handler->setServerInfo(*s_xbox_server_info);
      s_xbox_request_handler->setReturnEncodeType(RPCRequestHandler::Serialize);
      s_xbox_request_handler->incRequest();
    }
    return s_xbox_request_handler.get();
  }

  virtual void onThreadExit() {
    if (!s_xbox_request_handler.isNull()) {
      s_xbox_request_handler.destroy();
    }
  }
};

///////////////////////////////////////////////////////////////////////////////

static JobQueueDispatcher<XboxTransport*, XboxWorker> *s_dispatcher;

void XboxServer::Restart() {
  if (s_dispatcher) {
    s_dispatcher->stop();
    delete s_dispatcher;
    s_dispatcher = NULL;
  }

  if (RuntimeOption::XboxServerThreadCount > 0) {
    s_dispatcher = new JobQueueDispatcher<XboxTransport*, XboxWorker>
      (RuntimeOption::XboxServerThreadCount,
       RuntimeOption::ServerThreadRoundRobin,
       RuntimeOption::ServerThreadDropCacheTimeoutSeconds,
       RuntimeOption::ServerThreadDropStack,
       NULL);
    if (RuntimeOption::XboxServerLogInfo) {
      Logger::Info("xbox server started");
    }
    s_dispatcher->start();
  }
}

void XboxServer::Stop() {
  if (s_dispatcher) {
    s_dispatcher->stop();
    delete s_dispatcher;
    s_dispatcher = NULL;
  }
}

///////////////////////////////////////////////////////////////////////////////

static bool isLocalHost(CStrRef host) {
  return host.empty() || host == "localhost" || host == "127.0.0.1";
}

bool XboxServer::SendMessage(CStrRef message, Variant &ret, int timeout_ms,
                             CStrRef host /* = "localhost" */) {
  if (isLocalHost(host)) {

    if (RuntimeOption::XboxServerThreadCount <= 0) {
      return false;
    }

    XboxTransport *job = new XboxTransport(message);
    job->incRefCount(); // paired with worker's decRefCount()
    job->incRefCount(); // paired with decRefCount() at below
    assert(s_dispatcher);
    s_dispatcher->enqueue(job);

    if (timeout_ms <= 0) {
      timeout_ms = RuntimeOption::XboxDefaultLocalTimeoutMilliSeconds;
    }

    int code = 0;
    String response = job->getResults(code, timeout_ms);
    job->decRefCount(); // i'm done with this job

    if (code > 0) {
      ret.set("code", code);
      if (code == 200) {
        ret.set("response", f_unserialize(response));
      } else {
        ret.set("error", response);
      }
      return true;
    }

  } else { // remote

    string url = "http://";
    url += host.data();
    url += '/';
    url += RuntimeOption::XboxProcessMessageFunc;

    int timeoutSeconds = timeout_ms / 1000;
    if (timeoutSeconds <= 0) {
      timeoutSeconds = RuntimeOption::XboxDefaultRemoteTimeoutSeconds;
    }

    string hostStr(host.data());
    vector<string> headers;
    LibEventHttpClientPtr http =
      LibEventHttpClient::Get(hostStr, RuntimeOption::XboxServerPort);
    if (http->send(url, headers, timeoutSeconds, false,
                   message.data(), message.size())) {
      int code = http->getCode();
      if (code > 0) {
        int len = 0;
        char *response = http->recv(len);
        String sresponse(response, len, AttachString);
        ret.set("code", code);
        if (code == 200) {
          ret.set("response", f_unserialize(sresponse));
        } else {
          ret.set("error", sresponse);
        }
        return true;
      }
      // code wasn't correctly set by http client, treat it as not found
      ret.set("code", 404);
      ret.set("error", "http client failed");
    }
  }

  return false;
}

bool XboxServer::PostMessage(CStrRef message,
                             CStrRef host /* = "localhost" */) {
  if (isLocalHost(host)) {

    if (RuntimeOption::XboxServerThreadCount <= 0) {
      return false;
    }

    XboxTransport *job = new XboxTransport(message);
    job->incRefCount(); // paired with worker's decRefCount()
    assert(s_dispatcher);
    s_dispatcher->enqueue(job);
    return true;

  } else { // remote

    string url = "http://";
    url += host.data();
    url += "/xbox_post_message";

    vector<string> headers;
    string hostStr(host.data());
    LibEventHttpClientPtr http =
      LibEventHttpClient::Get(hostStr, RuntimeOption::XboxServerPort);
    if (http->send(url, headers, 0, false, message.data(), message.size())) {
      int code = http->getCode();
      if (code > 0) {
        int len = 0;
        char *response = http->recv(len);
        String sresponse(response, len, AttachString);
        if (code == 200 && same(f_unserialize(sresponse), true)) {
          return true;
        }
      }
    }
  }

  return false;
}

///////////////////////////////////////////////////////////////////////////////

class XboxTask : public SweepableResourceData {
public:
  DECLARE_OBJECT_ALLOCATION(XboxTask)

  XboxTask(CStrRef message, CStrRef reqInitDoc = "") {
    m_job = new XboxTransport(message, reqInitDoc);
    m_job->incRefCount();
  }

  ~XboxTask() {
    m_job->decRefCount();
  }

  XboxTransport *getJob() { return m_job;}

  static StaticString s_class_name;
  // overriding ResourceData
  virtual CStrRef o_getClassNameHook() const { return s_class_name; }

private:
  XboxTransport *m_job;
};
IMPLEMENT_OBJECT_ALLOCATION(XboxTask)

StaticString XboxTask::s_class_name("XboxTask");

///////////////////////////////////////////////////////////////////////////////

bool XboxServer::Available() {
  return s_dispatcher->getActiveWorker() <
         RuntimeOption::XboxServerThreadCount ||
         s_dispatcher->getQueuedJobs() <
         RuntimeOption::XboxServerMaxQueueLength;
}

Object XboxServer::TaskStart(CStrRef msg, CStrRef reqInitDoc /* = "" */) {
  bool xboxEnabled = (RuntimeOption::XboxServerThreadCount > 0);
  if (!xboxEnabled || !Available()) {
    const char* errMsg = (xboxEnabled ?
      "Cannot create new Xbox task because the Xbox queue has "
      "reached maximum capacity" :
      "Cannot create new Xbox task because the Xbox is not enabled");
    Object e = SystemLib::AllocExceptionObject(errMsg);
    throw_exception(e);
    return Object();
  }
  XboxTask *task = NEWOBJ(XboxTask)(msg, reqInitDoc);
  Object ret(task);
  XboxTransport *job = task->getJob();
  job->incRefCount(); // paired with worker's decRefCount()
  Transport *transport = g_context->getTransport();
  if (transport) {
    job->setHost(transport->getHeader("Host"));
  }
  assert(s_dispatcher);
  s_dispatcher->enqueue(job);

  return ret;
}

bool XboxServer::TaskStatus(CObjRef task) {
  XboxTask *ptask = task.getTyped<XboxTask>();
  return ptask->getJob()->isDone();
}

int XboxServer::TaskResult(CObjRef task, int timeout_ms, Variant &ret) {
  XboxTask *ptask = task.getTyped<XboxTask>();

  int code = 0;
  String response = ptask->getJob()->getResults(code, timeout_ms);
  if (code == 200) {
    ret = f_unserialize(response);
  } else {
    ret = response;
  }
  return code;
}

XboxServerInfoPtr XboxServer::GetServerInfo() {
  return *s_xbox_server_info;
}

RPCRequestHandler *XboxServer::GetRequestHandler() {
  if (s_xbox_request_handler.isNull()) return NULL;
  return s_xbox_request_handler.get();
}

///////////////////////////////////////////////////////////////////////////////
}
