#include <errno.h>

#include "common/Formatter.h"
#include "common/utf8.h"
#include "rgw_common.h"
#include "rgw_rados.h"
#include "rgw_formats.h"
#include "rgw_op.h"
#include "rgw_rest.h"
#include "rgw_rest_swift.h"
#include "rgw_rest_s3.h"
#include "rgw_swift_auth.h"

#include "rgw_formats.h"

#ifdef FASTCGI_INCLUDE_DIR
# include "fastcgi/fcgiapp.h"
#else
# include "fcgiapp.h"
#endif

#define dout_subsys ceph_subsys_rgw

static void dump_status(struct req_state *s, const char *status)
{
  CGI_PRINTF(s,"Status: %s\n", status);
}

void rgw_flush_formatter_and_reset(struct req_state *s, Formatter *formatter)
{
  std::ostringstream oss;
  formatter->flush(oss);
  std::string outs(oss.str());
  if (!outs.empty()) {
    CGI_PutStr(s, outs.c_str(), outs.size());
  }

  s->formatter->reset();
}

void rgw_flush_formatter(struct req_state *s, Formatter *formatter)
{
  std::ostringstream oss;
  formatter->flush(oss);
  std::string outs(oss.str());
  if (!outs.empty()) {
    CGI_PutStr(s, outs.c_str(), outs.size());
  }
}

void set_req_state_err(struct req_state *s, int err_no)
{
  const struct rgw_html_errors *r;

  if (err_no < 0)
    err_no = -err_no;
  s->err.ret = err_no;
  if (s->prot_flags & RGW_REST_SWIFT) {
    r = search_err(err_no, RGW_HTML_SWIFT_ERRORS, ARRAY_LEN(RGW_HTML_SWIFT_ERRORS));
    if (r) {
      s->err.http_ret = r->http_ret;
      s->err.s3_code = r->s3_code;
      return;
    }
  }
  r = search_err(err_no, RGW_HTML_ERRORS, ARRAY_LEN(RGW_HTML_ERRORS));
  if (r) {
    s->err.http_ret = r->http_ret;
    s->err.s3_code = r->s3_code;
    return;
  }
  dout(0) << "WARNING: set_req_state_err err_no=" << err_no << " resorting to 500" << dendl;

  s->err.http_ret = 500;
  s->err.s3_code = "UnknownError";
}

void dump_errno(struct req_state *s)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%d", s->err.http_ret);
  dump_status(s, buf);
}

void dump_errno(struct req_state *s, int err)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%d", err);
  dump_status(s, buf);
}

void dump_content_length(struct req_state *s, size_t len)
{
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu", (long unsigned int)len);
  CGI_PRINTF(s, "Content-Length: %s\n", buf);
  CGI_PRINTF(s, "Accept-Ranges: %s\n", "bytes");
}

void dump_etag(struct req_state *s, const char *etag)
{
  if (s->prot_flags & RGW_REST_SWIFT)
    CGI_PRINTF(s,"etag: %s\n", etag);
  else
    CGI_PRINTF(s,"ETag: \"%s\"\n", etag);
}

void dump_last_modified(struct req_state *s, time_t t)
{

  char timestr[TIME_BUF_SIZE];
  struct tm *tmp = gmtime(&t);
  if (tmp == NULL)
    return;

  if (strftime(timestr, sizeof(timestr), "%a, %d %b %Y %H:%M:%S %Z", tmp) == 0)
    return;

  CGI_PRINTF(s, "Last-Modified: %s\n", timestr);
}

void dump_time(struct req_state *s, const char *name, time_t *t)
{
  char buf[TIME_BUF_SIZE];
  struct tm result;
  struct tm *tmp = gmtime_r(t, &result);
  if (tmp == NULL)
    return;

  if (strftime(buf, sizeof(buf), "%Y-%m-%dT%T.000Z", tmp) == 0)
    return;

  s->formatter->dump_string(name, buf);
}

void dump_owner(struct req_state *s, string& id, string& name, const char *section)
{
  if (!section)
    section = "Owner";
  s->formatter->open_object_section(section);
  s->formatter->dump_string("ID", id);
  s->formatter->dump_string("DisplayName", name);
  s->formatter->close_section();
}

void dump_start(struct req_state *s)
{
  if (!s->content_started) {
    if (s->format == RGW_FORMAT_XML)
      s->formatter->write_raw_data(XMLFormatter::XML_1_DTD);
    s->content_started = true;
  }
}

void end_header(struct req_state *s, const char *content_type)
{
  string ctype;

  if (!content_type || s->err.is_err()) {
    switch (s->format) {
    case RGW_FORMAT_XML:
      ctype = "application/xml";
      break;
    case RGW_FORMAT_JSON:
      ctype = "application/json";
      break;
    default:
      ctype = "text/plain";
      break;
    }
    if (s->prot_flags & RGW_REST_SWIFT)
      ctype.append("; charset=utf-8");
    content_type = ctype.c_str();
  }
  if (s->err.is_err()) {
    dump_start(s);
    s->formatter->open_object_section("Error");
    if (!s->err.s3_code.empty())
      s->formatter->dump_string("Code", s->err.s3_code);
    if (!s->err.message.empty())
      s->formatter->dump_string("Message", s->err.message);
    s->formatter->close_section();
    dump_content_length(s, s->formatter->get_len());
  }
  CGI_PRINTF(s,"Content-type: %s\r\n\r\n", content_type);
  s->header_ended = true;
  rgw_flush_formatter_and_reset(s, s->formatter);
}

void abort_early(struct req_state *s, int err_no)
{
  set_req_state_err(s, err_no);
  dump_errno(s);
  end_header(s);
  rgw_flush_formatter_and_reset(s, s->formatter);
  perfcounter->inc(l_rgw_failed_req);
}

void dump_continue(struct req_state *s)
{
  dump_status(s, "100");
  FCGX_FFlush(s->fcgx->out);
}

void dump_range(struct req_state *s, uint64_t ofs, uint64_t end, uint64_t total)
{
  char range_buf[128];

  /* dumping range into temp buffer first, as libfcgi will fail to digest %lld */
  snprintf(range_buf, sizeof(range_buf), "%lld-%lld/%lld", (long long)ofs, (long long)end, (long long)total);
  CGI_PRINTF(s,"Content-Range: bytes %s\n", range_buf);
}

int RGWGetObj_REST::get_params()
{
  range_str = s->env->get("HTTP_RANGE");
  if_mod = s->env->get("HTTP_IF_MODIFIED_SINCE");
  if_unmod = s->env->get("HTTP_IF_UNMODIFIED_SINCE");
  if_match = s->env->get("HTTP_IF_MATCH");
  if_nomatch = s->env->get("HTTP_IF_NONE_MATCH");

  return 0;
}


int RGWPutObj_REST::verify_params()
{
  if (s->length) {
    off_t len = atoll(s->length);
    if (len > (off_t)RGW_MAX_PUT_SIZE) {
      return -ERR_TOO_LARGE;
    }
  }

  return 0;
}

int RGWPutObj_REST::get_params()
{
  supplied_md5_b64 = s->env->get("HTTP_CONTENT_MD5");

  return 0;
}

int RGWPutObj_REST::get_data(bufferlist& bl)
{
  size_t cl;
  if (s->length) {
    cl = atoll(s->length) - ofs;
    if (cl > RGW_MAX_CHUNK_SIZE)
      cl = RGW_MAX_CHUNK_SIZE;
  } else {
    cl = RGW_MAX_CHUNK_SIZE;
  }

  int len = 0;
  if (cl) {
    bufferptr bp(cl);

    CGI_GetStr(s, bp.c_str(), cl, len);
    bl.append(bp);
  }

  if ((uint64_t)ofs + len > RGW_MAX_PUT_SIZE) {
    return -ERR_TOO_LARGE;
  }

  if (!ofs)
    supplied_md5_b64 = s->env->get("HTTP_CONTENT_MD5");

  return len;
}

int RGWPutACLs_REST::get_params()
{
  size_t cl = 0;
  if (s->length)
    cl = atoll(s->length);
  if (cl) {
    data = (char *)malloc(cl + 1);
    if (!data) {
       ret = -ENOMEM;
       return ret;
    }
    CGI_GetStr(s, data, cl, len);
    data[len] = '\0';
  } else {
    len = 0;
  }

  return ret;
}

int RGWInitMultipart_REST::get_params()
{
  if (!s->args.exists("uploads")) {
    ret = -ENOTSUP;
  }

  return ret;
}

static int read_all_chunked_input(req_state *s, char **pdata, int *plen)
{
#define READ_CHUNK 4096
#define MAX_READ_CHUNK (128 * 1024)
  int need_to_read = READ_CHUNK;
  int total = need_to_read;
  char *data = (char *)malloc(total + 1);
  if (!data)
    return -ENOMEM;

  int read_len = 0, len = 0;
  do {
    CGI_GetStr(s, data + len, need_to_read, read_len);

    len += read_len;

    if (read_len == need_to_read) {
      if (need_to_read < MAX_READ_CHUNK)
	need_to_read *= 2;

      total += need_to_read;

      void *p = realloc(data, total + 1);
      if (!p) {
        free(data);
        return -ENOMEM;
      }
      data = (char *)p;
    } else {
      break;
    }

  } while (true);
  data[len] = '\0';

  *pdata = data;
  *plen = len;

  return 0;
}

int RGWCompleteMultipart_REST::get_params()
{
  upload_id = s->args.get("uploadId");

  if (upload_id.empty()) {
    ret = -ENOTSUP;
    return ret;
  }
  size_t cl = 0;

  if (s->length)
    cl = atoll(s->length);
  if (cl) {
    data = (char *)malloc(cl + 1);
    if (!data) {
       ret = -ENOMEM;
       return ret;
    }
    CGI_GetStr(s, data, cl, len);
    data[len] = '\0';
  } else {
    const char *encoding = s->env->get("HTTP_TRANSFER_ENCODING");
    if (!encoding || strcmp(encoding, "chunked") != 0)
      return -ERR_LENGTH_REQUIRED;

    ret = read_all_chunked_input(s, &data, &len);
    if (ret < 0)
      return ret;
  }

  return ret;
}

int RGWListMultipart_REST::get_params()
{
  upload_id = s->args.get("uploadId");

  if (upload_id.empty()) {
    ret = -ENOTSUP;
  }
  string str = s->args.get("part-number-marker");
  if (!str.empty())
    marker = atoi(str.c_str());
  
  str = s->args.get("max-parts");
  if (!str.empty())
    max_parts = atoi(str.c_str());

  return ret;
}

int RGWListBucketMultiparts_REST::get_params()
{
  delimiter = s->args.get("delimiter");
  prefix = s->args.get("prefix");
  string str = s->args.get("max-parts");
  if (!str.empty())
    max_uploads = atoi(str.c_str());
  else
    max_uploads = default_max;

  string key_marker = s->args.get("key-marker");
  string upload_id_marker = s->args.get("upload-id-marker");
  if (!key_marker.empty())
    marker.init(key_marker, upload_id_marker);

  return 0;
}

int RGWDeleteMultiObj_REST::get_params()
{
  bucket_name = s->bucket_name;

  if (bucket_name.empty()) {
    ret = -EINVAL;
    return ret;
  }

  // everything is probably fine, set the bucket
  bucket = s->bucket;

  size_t cl = 0;

  if (s->length)
    cl = atoll(s->length);
  if (cl) {
    data = (char *)malloc(cl + 1);
    if (!data) {
      ret = -ENOMEM;
      return ret;
    }
    CGI_GetStr(s, data, cl, len);
    data[len] = '\0';
  } else {
    return -EINVAL;
  }

  return ret;
}

static void line_unfold(const char *line, string& sdest)
{
  char dest[strlen(line) + 1];
  const char *p = line;
  char *d = dest;

  while (isspace(*p))
    ++p;

  bool last_space = false;

  while (*p) {
    switch (*p) {
    case '\n':
    case '\r':
      *d = ' ';
      if (!last_space)
        ++d;
      last_space = true;
      break;
    default:
      *d = *p;
      ++d;
      last_space = false;
      break;
    }
    ++p;
  }
  *d = 0;
  sdest = dest;
}

struct str_len {
  const char *str;
  int len;
};

#define STR_LEN_ENTRY(s) { s, sizeof(s) - 1 }

struct str_len meta_prefixes[] = { STR_LEN_ENTRY("HTTP_X_AMZ"),
                                   STR_LEN_ENTRY("HTTP_X_GOOG"),
                                   STR_LEN_ENTRY("HTTP_X_DHO"),
                                   STR_LEN_ENTRY("HTTP_X_RGW"),
                                   STR_LEN_ENTRY("HTTP_X_OBJECT"),
                                   STR_LEN_ENTRY("HTTP_X_CONTAINER"),
                                   {NULL, 0} };

static int init_meta_info(struct req_state *s)
{
  const char *p;

  s->x_meta_map.clear();

  for (int i=0; (p = s->fcgx->envp[i]); ++i) {
    const char *prefix;
    for (int prefix_num = 0; (prefix = meta_prefixes[prefix_num].str) != NULL; prefix_num++) {
      int len = meta_prefixes[prefix_num].len;
      if (strncmp(p, prefix, len) == 0) {
        dout(10) << "meta>> " << p << dendl;
        const char *name = p+len; /* skip the prefix */
        const char *eq = strchr(name, '=');
        if (!eq) /* shouldn't happen! */
          continue;
        int name_len = eq - name;

        if (strncmp(name, "_META_", name_len) == 0)
          s->has_bad_meta = true;

        char name_low[meta_prefixes[0].len + name_len + 1];
        snprintf(name_low, meta_prefixes[0].len - 5 + name_len + 1, "%s%s", meta_prefixes[0].str + 5 /* skip HTTP_ */, name); // normalize meta prefix
        int j;
        for (j = 0; name_low[j]; j++) {
          if (name_low[j] != '_')
            name_low[j] = tolower(name_low[j]);
          else
            name_low[j] = '-';
        }
        name_low[j] = 0;
        string val;
        line_unfold(eq + 1, val);

        map<string, string>::iterator iter;
        iter = s->x_meta_map.find(name_low);
        if (iter != s->x_meta_map.end()) {
          string old = iter->second;
          int pos = old.find_last_not_of(" \t"); /* get rid of any whitespaces after the value */
          old = old.substr(0, pos + 1);
          old.append(",");
          old.append(val);
          s->x_meta_map[name_low] = old;
        } else {
          s->x_meta_map[name_low] = val;
        }
      }
    }
  }
  map<string, string>::iterator iter;
  for (iter = s->x_meta_map.begin(); iter != s->x_meta_map.end(); ++iter) {
    dout(10) << "x>> " << iter->first << ":" << iter->second << dendl;
  }

  return 0;
}

// This function enforces Amazon's spec for bucket names.
// (The requirements, not the recommendations.)
int RGWHandler_REST::validate_bucket_name(const string& bucket)
{
  int len = bucket.size();
  if (len < 3) {
    if (len == 0) {
      // This request doesn't specify a bucket at all
      return 0;
    }
    // Name too short
    return -ERR_INVALID_BUCKET_NAME;
  }
  else if (len > 255) {
    // Name too long
    return -ERR_INVALID_BUCKET_NAME;
  }

  return 0;
}

// "The name for a key is a sequence of Unicode characters whose UTF-8 encoding
// is at most 1024 bytes long."
// However, we can still have control characters and other nasties in there.
// Just as long as they're utf-8 nasties.
int RGWHandler_REST::validate_object_name(const string& object)
{
  int len = object.size();
  if (len > 1024) {
    // Name too long
    return -ERR_INVALID_OBJECT_NAME;
  }

  if (check_utf8(object.c_str(), len)) {
    // Object names must be valid UTF-8.
    return -ERR_INVALID_OBJECT_NAME;
  }
  return 0;
}

static http_op op_from_method(const char *method)
{
  if (!method)
    return OP_UNKNOWN;
  if (strcmp(method, "GET") == 0)
    return OP_GET;
  if (strcmp(method, "PUT") == 0)
    return OP_PUT;
  if (strcmp(method, "DELETE") == 0)
    return OP_DELETE;
  if (strcmp(method, "HEAD") == 0)
    return OP_HEAD;
  if (strcmp(method, "POST") == 0)
    return OP_POST;
  if (strcmp(method, "COPY") == 0)
    return OP_COPY;

  return OP_UNKNOWN;
}

int RGWHandler_REST::preprocess(struct req_state *s, FCGX_Request *fcgx)
{
  s->fcgx = fcgx;
  s->request_uri = s->env->get("REQUEST_URI");
  int pos = s->request_uri.find('?');
  if (pos >= 0) {
    s->request_params = s->request_uri.substr(pos + 1);
    s->request_uri = s->request_uri.substr(0, pos);
  }
  url_decode(s->request_uri, s->decoded_uri);
  s->method = s->env->get("REQUEST_METHOD");
  s->host = s->env->get("HTTP_HOST");
  s->length = s->env->get("CONTENT_LENGTH");
  if (s->length) {
    if (*s->length == '\0')
      return -EINVAL;
    s->content_length = atoll(s->length);
  }

  s->content_type = s->env->get("CONTENT_TYPE");
  s->http_auth = s->env->get("HTTP_AUTHORIZATION");

  if (g_conf->rgw_print_continue) {
    const char *expect = s->env->get("HTTP_EXPECT");
    s->expect_cont = (expect && !strcasecmp(expect, "100-continue"));
  }
  s->op = op_from_method(s->method);

  init_meta_info(s);

  return 0;
}

int RGWHandler_REST::read_permissions(RGWOp *op_obj)
{
  bool only_bucket;

  switch (s->op) {
  case OP_HEAD:
  case OP_GET:
    only_bucket = false;
    break;
  case OP_PUT:
  case OP_POST:
    /* is it a 'multi-object delete' request? */
    if (s->request_params == "delete") {
      only_bucket = true;
      break;
    }
    if (is_obj_update_op()) {
      only_bucket = false;
      break;
    }
    /* is it a 'create bucket' request? */
    if (s->object_str.size() == 0)
      return 0;
  case OP_DELETE:
    only_bucket = true;
    break;
  case OP_COPY: // op itself will read and verify the permissions
    return 0;
  default:
    return -EINVAL;
  }

  return do_read_permissions(op_obj, only_bucket);
}

RGWOp *RGWHandler_REST::get_op()
{
  RGWOp *op;
  switch (s->op) {
   case OP_GET:
     op = get_retrieve_op(true);
     break;
   case OP_PUT:
     op = get_create_op();
     break;
   case OP_DELETE:
     op = get_delete_op();
     break;
   case OP_HEAD:
     op = get_retrieve_op(false);
     break;
   case OP_POST:
     op = get_post_op();
     break;
   case OP_COPY:
     op = get_copy_op();
     break;
   default:
     return NULL;
  }

  if (op) {
    op->init(s, this);
  }
  return op;
}


RGWRESTMgr::RGWRESTMgr()
{
  // order is important!
  protocol_handlers.push_back(new RGWHandler_REST_SWIFT);
  protocol_handlers.push_back(new RGWHandler_SWIFT_Auth);
  protocol_handlers.push_back(new RGWHandler_REST_S3);
}

RGWRESTMgr::~RGWRESTMgr()
{
  vector<RGWHandler *>::iterator iter;
  for (iter = protocol_handlers.begin(); iter != protocol_handlers.end(); ++iter) {
    delete *iter;
  }
}

RGWHandler *RGWRESTMgr::get_handler(struct req_state *s, FCGX_Request *fcgx,
				    int *init_error)
{
  RGWHandler *handler;

  *init_error = RGWHandler_REST::preprocess(s, fcgx);
  if (*init_error < 0)
    return NULL;

  vector<RGWHandler *>::iterator iter;
  for (iter = protocol_handlers.begin(); iter != protocol_handlers.end(); ++iter) {
    handler = *iter;
    if (handler->filter_request(s))
      break;
  }
  if (iter == protocol_handlers.end())
    return NULL;

  *init_error = handler->init(s, fcgx);
  if (*init_error < 0)
    return NULL;

  return handler;
}

void RGWHandler_REST::put_op(RGWOp *op)
{
  delete op;
}

