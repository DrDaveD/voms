/*********************************************************************
 *
 * Authors: Vincenzo Ciaschini - Vincenzo.Ciaschini@cnaf.infn.it
 *
 * Copyright (c) 2002, 2003 INFN-CNAF on behalf of the EU DataGrid.
 * For license conditions see LICENSE file or
 * http://www.edg.org/license.html
 *
 * Parts of this code may be based upon or even include verbatim pieces,
 * originally written by other people, in which case the original header
 * follows.
 *
 *********************************************************************/
#include "config.h"

extern "C" {
#include "replace.h"
#include "uuid.h"

#define SUBPACKAGE "voms"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <dlfcn.h>

#include <openssl/evp.h>
#include "newformat.h"
#include "init.h"
#include "gssapi.h"
#include "credentials.h"

#include "log.h"
#include "streamers.h"

static int reload = 0;

void *logh = NULL;
}


#include "Server.h"

#include "VOMSServer.h"

#include "options.h"
#include "data.h"
#include "pass.h"
#include "errors.h"
#include "vomsxml.h"

#include <map>
#include <set>
#include <string>
#include <algorithm>
#include <iostream>

#include "attribute.h"

#include "dbwrap.h"

#include "voms_api.h"

#ifdef HAVE_GLOBUS_MODULE_ACTIVATE
#include <globus_module.h>
#include <globus_openssl.h>
#endif

extern int AC_Init(void);

#include "ccwrite.h"

extern "C" {
  extern char *get_error(int);
}

static const int DEFAULT_PORT    = 15000;
static const int DEFAULT_TIMEOUT = 60;

sqliface::interface *db = NULL;

typedef std::map<std::string, int> ordermap;

static ordermap ordering;

static std::string firstfqan="";

static std::string sqllib = "";

typedef sqliface::interface* (*cdb)();
typedef int (*gv)();

cdb NewDB;
gv  getlibversion;

bool compat_flag = false;
bool short_flags = false;

static bool determine_group_and_role(std::string command, char *comm, char **group,
                                     char **role);

static void
sigchld_handler(int sig)
{
  int save_errno = errno;
  pid_t pid;
  int status;

  while ((pid = waitpid(-1, &status, WNOHANG)) > 0 ||
         (pid < 0 && errno == EINTR))
    ;

  signal(SIGCHLD, sigchld_handler);
  errno = save_errno;
}
static BIGNUM *get_serial();

static void
sighup_handler(int sig)
{
  reload = 1;
}

static bool compare(const std::string &lhs, const std::string &rhs)
{
  ordermap::iterator lhi=ordering.find(lhs);
  ordermap::iterator rhi=ordering.find(rhs);

  LOGM(VARP, logh, LEV_DEBUG, T_PRE, "Comparing: %s to %s", lhs.c_str(), rhs.c_str());
  if (lhi == ordering.end()) {
    LOG(logh, LEV_DEBUG, T_PRE, "No left hand side");
    return false;
  }
  if (rhi == ordering.end()) {
    LOG(logh, LEV_DEBUG, T_PRE, "No Right hand side");
    return true;
  }
  LOGM(VARP, logh, LEV_DEBUG, T_PRE, "%d:%d",lhi->second, rhi->second);
  return (lhi->second < rhi->second);
}

static void orderattribs(std::vector<std::string> &v)
{
  std::partial_sort(v.begin(), v.begin()+ordering.size(), v.end(), compare);
}

static void parse_order(const std::string &message, ordermap &ordering)
{
  int order = 0;
  std::string::size_type position = 0;
  bool init = true;

  LOGM(VARP, logh, LEV_DEBUG, T_PRE, "Initiating parse order: %s",message.c_str());
  while (position != std::string::npos) {
    LOG(logh, LEV_DEBUG, T_PRE, "Entered loop");

    if (init) {
      position = 0;
      init = false;
    }
    else
      position++;

    /* There is a specified ordering */
    std::string::size_type end_token = message.find_first_of(',', position);
    std::string attribute;
    if (end_token == std::string::npos)
      attribute = message.substr(position);
    else
      attribute = message.substr(position, end_token - position);
    LOGM(VARP, logh, LEV_DEBUG, T_PRE, "Attrib: %s",attribute.c_str());
    std::string::size_type divider = attribute.find(':');
    std::string fqan;

    if (divider == std::string::npos) {
      fqan = attribute;
      if (firstfqan.empty()) {
        firstfqan = fqan;
      }
    }
    else {
      fqan = attribute.substr(0, divider) +
        "/Role=" + attribute.substr(divider+1);

      if (firstfqan.empty())
        firstfqan = fqan;
    }

    LOGM(VARP, logh, LEV_DEBUG, T_PRE, "Order: %s",fqan.c_str());
    ordering.insert(std::make_pair<std::string, int>(fqan,order));
    order++;
    position = end_token;
  }
}

static void parse_targets(const std::string &message,
                          std::vector<std::string> &target)
{
  std::string::size_type position = 0;

  bool init = true;

  while (position != std::string::npos) {
    if (!init)
      position++;
    else
      init = false;

    /* There is a specified ordering */
    std::string::size_type end_token = message.find_first_of(',',position);
    std::string attribute;
    if (end_token == std::string::npos)
      attribute = message.substr(position);
    else
      attribute = message.substr(position, end_token - position);
    target.push_back(attribute);
    position = end_token;
  }
}

bool not_in(std::string fqan, std::vector<std::string> fqans)
{
  return (find(fqans.begin(), fqans.end(), fqan) == fqans.end());
}

VOMSServer::VOMSServer(int argc, char *argv[]) : sock(0,0,NULL,50,false),
                                                 validity(86400),
                                                 logfile("/var/log/voms"),
                                                 gatekeeper_test(false),
                                                 daemon_port(DEFAULT_PORT),
                                                 foreground(false),
                                                 globuspwd(""), globusid(""),
                                                 x509_cert_dir(""),
                                                 x509_cert_file(""),
                                                 x509_user_proxy(""),
                                                 x509_user_cert(""),
                                                 x509_user_key(""),
                                                 desired_name_char(""),
                                                 username("voms"),
                                                 dbname("voms"),
                                                 contactstring(""),
                                                 mysql_port(0),
                                                 mysql_socket(""),
                                                 passfile(""),
                                                 voname("unspecified"),
                                                 uri(""), version(0),
                                                 subject(""), ca(""),
                                                 debug(false), code(-1),
                                                 backlog(50), logger(NULL),
                                                 socktimeout(-1),
                                                 logmax(10000000),loglev(2),
                                                 logt(T_STARTUP|T_REQUEST|T_RESULT),
                                                 logdf("%c"),
                                                 logf("%d:%h:%s(%p):%V:%T:%F (%f:%l):%m"),
                                                 newformat(false),
                                                 insecure(false),
                                                 shortfqans(false),
                                                 do_syslog(false),
                                                 base64encoding(false)
{
  struct stat statbuf;

  signal(SIGCHLD, sigchld_handler);
  ac = argc;
  av = argv;

  if ((stat("/etc/nologin", &statbuf)) == 0)
    throw VOMSInitException("/etc/nologin present\n");

#define PROXYCERTINFO_V3      "1.3.6.1.4.1.3536.1.222"
#define PROXYCERTINFO_V4      "1.3.6.1.5.5.7.1.14"
#define OBJC(c,n) OBJ_create(c,n,#c)

  /* Proxy Certificate Extension's related objects */
  OBJC(PROXYCERTINFO_V3, "PROXYCERTINFO_V3");
  OBJC(PROXYCERTINFO_V4, "PROXYCERTINFO_V4");


  std::string fakeuri = "";
  bool progversion = false;

  struct option opts[] = {
    {"help",            0, NULL,                      OPT_HELP},
    {"usage",           0, NULL,                      OPT_HELP},
    {"test",            0, (int *)&gatekeeper_test,   OPT_BOOL},
    {"conf",            1, NULL,                      OPT_CONFIG},
    {"port",            1, &daemon_port,              OPT_NUM},
    {"logfile",         1, (int *)&logfile,           OPT_STRING},
    {"globusid",        1, (int *)&globusid,          OPT_STRING},
    {"globuspwd",       1, (int *)&globuspwd,         OPT_STRING},
    {"x509_cert_dir",   1, (int *)&x509_cert_dir,     OPT_STRING},
    {"x509_cert_file",  1, (int *)&x509_cert_file,    OPT_STRING},
    {"x509_user_proxy", 1, (int *)&x509_user_proxy,   OPT_STRING},
    {"x509_user_cert",  1, (int *)&x509_user_cert,    OPT_STRING},
    {"x509_user_key",   1, (int *)&x509_user_key,     OPT_STRING},
    {"desired_name",    1, (int *)&desired_name_char, OPT_STRING},
    {"foreground",      0, (int *)&foreground,        OPT_BOOL},
    {"username",        1, (int *)&username,          OPT_STRING},
    {"timeout",         1, &validity,                 OPT_NUM},
    {"dbname",          1, (int *)&dbname,            OPT_STRING},
    {"contactstring",   1, (int *)&contactstring,     OPT_STRING},
    {"mysql-port",      1, (int *)&mysql_port,        OPT_NUM},
    {"mysql-socket",    1, (int *)&mysql_socket,      OPT_STRING},
    {"passfile",        1, (int *)&passfile,          OPT_STRING},
    {"vo",              1, (int *)&voname,            OPT_STRING},
    {"uri",             1, (int *)&fakeuri,           OPT_STRING},
    {"globus",          1, &version,                  OPT_NUM},
    {"version",         0, (int *)&progversion,       OPT_BOOL},
    {"backlog",         1, &backlog,                  OPT_NUM},
    {"debug",           0, (int *)&debug,             OPT_BOOL},
    {"code",            1, &code,                     OPT_NUM},
    {"loglevel",        1, &loglev,                   OPT_NUM},
    {"logtype",         1, &logt,                     OPT_NUM},
    {"logformat",       1, (int *)&logf,              OPT_STRING},
    {"logdateformat",   1, (int *)&logdf,             OPT_STRING},
    {"sqlloc",          1, (int *)&sqllib,            OPT_STRING},
    {"compat",          1, (int *)&compat_flag,       OPT_BOOL},
    {"socktimeout",     1, &socktimeout,              OPT_NUM},
    {"logmax",          1, &logmax,                   OPT_NUM},
    {"newformat",       1, (int *)&newformat,         OPT_BOOL},
    {"skipcacheck",     1, (int *)&insecure,          OPT_BOOL},
    {"shortfqans",      0, (int *)&shortfqans,        OPT_BOOL},
    {"syslog",          0, (int *)&do_syslog,         OPT_BOOL},
    {"base64",          0, (int *)&base64encoding,    OPT_BOOL},
    {0, 0, 0, 0}
  };

  /*
   * Parse the command line arguments
   */

  set_usage("[-help] [-usage] [-conf parmfile] [-foreground] [-port port]\n"
            "[-logfile file] [-passfile file] [-vo voname]\n"
            "[-globusid globusid] [-globuspwd file] [-globus version]\n"
            "[-x509_cert_dir path] [-x509_cert_file file]\n"
            "[-x509_user_cert file] [-x509_user_key file]\n"
            "[-dbname name] [-username name] [-contactstring name]\n"
            "[-mysql-port port] [-mysql-socket socket] [-timeout limit]\n"
            "[-x509_user_proxy file] [-test] [-uri uri] [-code num]\n"
            "[-loglevel lev] [-logtype type] [-logformat format]\n"
            "[-logdateformat format] [-debug] [-backlog num] [-skipcacheck]\n"
            "[-version][-sqlloc path][-compat][-logmax n][-socktimeout n]\n"
            "[-shortfqans]\n");

  if (!getopts(argc, argv, opts))
    throw VOMSInitException("unable to read options");


  short_flags = shortfqans;

  if (socktimeout == -1 && debug)
    socktimeout = 0;
  else
    socktimeout = DEFAULT_TIMEOUT;

  if (code == -1)
    code = daemon_port;

  if (progversion) {
    std::cout << SUBPACKAGE << "\nVersion: " << VERSION << std::endl;
    std::cout << "Compiled: " << __DATE__ << " " << __TIME__ << std::endl;
    exit(0);
  }

  if ((logh = LogInit())) {
    //    if ((logger = FileNameStreamerAdd(logh, logfile.c_str(), logmax, code, 0))) {
      loglevels lev;

      switch(loglev) {
      case 1: lev = LEV_NONE; break;
      case 2: lev = LEV_ERROR; break;
      case 3: lev = LEV_WARN; break;
      case 4: lev = LEV_INFO; break;
      case 5: lev = LEV_DEBUG; break;
      default: lev = LEV_DEBUG; break;
      }
      if (debug)
        lev = LEV_DEBUG;

      if (lev == LEV_DEBUG)
        logt = T_STARTUP|T_REQUEST|T_RESULT;

      (void)LogLevel(logh, lev);
      (void)LogType(logh, logt);
      (void)SetCurLogType(logh, T_STARTUP);
      (void)LogService(logh, "vomsd");
      (void)LogFormat(logh, logf.c_str());
      //      (void)LogDateFormat(logh, logdf.c_str());
      (void)StartLogger(logh, code);
      (void)LogActivate(logh, "FILE");
      if (do_syslog)
        (void)LogActivate(logh, "SYSLOG");

      (void)LogOption(logh, "NAME", logfile.c_str());
      (void)LogOptionInt(logh, "MAXSIZE", logmax);
      (void)LogOption(logh, "DATEFORMAT", logdf.c_str());
      //    }
  }
  else
    throw VOMSInitException("logging startup failure");

  if (debug) {
    LOGM(VARP, logh, LEV_INFO, T_PRE, "Package: %s", SUBPACKAGE);
    LOGM(VARP, logh, LEV_INFO, T_PRE, "Version: %s", VERSION);
    LOGM(VARP, logh, LEV_INFO, T_PRE, "Compiled: %s %s", __DATE__, __TIME__);
    for (int i = 0; i < argc; i++)
      LOGM(VARP, logh, LEV_DEBUG, T_PRE, "argv[%d] = \"%s\"", i, argv[i]);
  }

  if (!sqllib.empty()) {
    void * library = dlopen(sqllib.c_str(), RTLD_LAZY);
    if(!library) {
      LOG(logh, LEV_ERROR, T_PRE, ((std::string)("Cannot load library: " + sqllib)).c_str());
      std::cout << "Cannot load library: "<< sqllib << std::endl;
      std::cout << dlerror() << std::endl;
      exit(1);
    }

    getlibversion = (gv)dlsym(library, "getDBInterfaceVersion");
    if (!getlibversion || getlibversion() != 3) {
      LOGM(VARP, logh, LEV_ERROR, T_PRE, "Old version of interface library found. Expecting >= 3, Found: %d", 
           (getlibversion ? getlibversion() : 1));
      std::cout << "Old version of interface library found. Expecting >= 3, Found: " << 
        (getlibversion ? getlibversion() : 1);
      exit(1);
    }

    NewDB = (cdb)dlsym(library, "CreateDB");
    if (!NewDB) {
      LOG(logh, LEV_ERROR, T_PRE, ((std::string)("Cannot find initialization symbol in: " + sqllib)).c_str());
      std::cout << "Cannot find initialization symbol in: "<< sqllib << dlerror() << std::endl;
      exit(1);
    }

  }
  else {
    std::cout << "Cannot load library! "<< std::endl;
    LOG(logh, LEV_ERROR, T_PRE, "Cannot load library!" );
    exit(1);
  }

  if (!getpasswd(passfile, logh))  {
    LOG(logh, LEV_ERROR, T_PRE, "can't read password file!\n");
    throw VOMSInitException("can't read password file!");
  }

  if(contactstring.empty())
    contactstring = (std::string)"localhost";

  db = NewDB();

  if (!db) {
    LOG(logh, LEV_ERROR, T_PRE, "Cannot initialize DB library.");
    std::cout << "Cannot initialize DB library.";
    exit(1);
  }

  db->setOption(OPTION_SET_PORT, &mysql_port);
  if (!mysql_socket.empty())
    db->setOption(OPTION_SET_SOCKET, (void*)mysql_socket.c_str());
  db->setOption(OPTION_SET_INSECURE, &insecure);

  if (!db->connect(dbname.c_str(), contactstring.c_str(), 
                   username.c_str(), passwd())) {
    LOGM(VARP, logh, LEV_ERROR, T_PRE, "Unable to connect to database: %s", 
         db->errorMessage());
    std::cout << "Unable to connect to database: " <<
      db->errorMessage() << std::endl;
    exit(1);
  }

  int v = 0;
  sqliface::interface *session = db->getSession();
  bool result = session->operation(OPERATION_GET_VERSION, &v, NULL);
  std::string errormessage = session->errorMessage();
  db->releaseSession(session);

  if (result) {
    if (v < 2) {
      LOGM(VARP, logh, LEV_ERROR, T_PRE, "Detected DB Version: %d. Required DB version >= 2", v);
      std::cerr << "Detected DB Version: " << v << ". Required DB version >= 2";
      throw VOMSInitException("wrong database version");
    }
  }
  else {
    LOGM(VARP, logh, LEV_ERROR, T_PRE, (std::string("Error connecting to the database : ") + errormessage).c_str());
    throw VOMSInitException((std::string("Error connecting to the database : ") + errormessage));
  }


  version = globus(version);
  if (version == 0) {
    std::cerr << "Unable to discover Globus Version: Trying for 2.2"
              << std::endl;
    LOG(logh, LEV_WARN, T_PRE, "Unable to discover Globus Version: Trying for 2.2");
    version = 22;
  }

  if (fakeuri.empty()) {
    int   ok;

    int   hostnamesize = 50;
    char *hostname = new char[1];
    do {
      delete[] hostname;
      hostname = new char[hostnamesize];
      ok = gethostname(hostname, hostnamesize);
      hostnamesize += 50;
    } while (ok);
    std::string temp;

    uri = std::string(hostname) + ":" + stringify(daemon_port, temp);
    delete[] hostname;
  }
  else
    uri = fakeuri;

  sock = GSISocketServer(daemon_port, version, NULL, backlog);

  setenv("GLOBUSID", globusid.c_str(), 1);

  /*
   * Dont use default env proxy cert for gatekeeper if run as root
   * this might get left over. You can still use -x509_user_proxy
   */

  unsetenv("X509_USER_PROXY");

  if (!globuspwd.empty()) {
    setenv("GLOBUSPWD", globuspwd.c_str(), 1);
  }

  if (!x509_cert_dir.empty()) {
    setenv("X509_CERT_DIR", x509_cert_dir.c_str(), 1);
  }
  if (!x509_cert_file.empty()) {
    setenv("X509_CERT_FILE", x509_cert_file.c_str(), 1);
  }
  if (!x509_user_proxy.empty()) {
    setenv("X509_USER_PROXY", x509_user_proxy.c_str(), 1);
  }
  if (!x509_user_cert.empty()) {
    setenv("X509_USER_CERT", x509_user_cert.c_str(), 1);
  }
  if (!x509_user_key.empty()) {
    setenv("X509_USER_KEY", x509_user_key.c_str(), 1);
  }

  sock.SetFlags(GSS_C_MUTUAL_FLAG | GSS_C_CONF_FLAG | GSS_C_INTEG_FLAG);
  sock.SetLogger(logh);
  std::string msg = "URI: " + uri;

  LOGM(VARP, logh, LEV_INFO, T_PRE, "URI: %s", uri.c_str());
  LOGM(VARP,  logh, LEV_INFO, T_PRE, "Detected Globus Version: %d", version);

  AC_Init();
}

VOMSServer::~VOMSServer() {}

void VOMSServer::Run()
{
  pid_t pid = 0;

  if (!debug)
    if (daemon(0,0))
      exit(0);

  try {
    signal(SIGHUP, sighup_handler);
    LOG(logh, LEV_DEBUG, T_PRE, "Trying to open socket.");
    sock.Open();
    sock.SetTimeout(socktimeout);

    for (;;) {

      if (reload) {
        reload=0;
        UpdateOpts();
      }

      if (sock.Listen()) {
        if (reload) {
          reload=0;
          UpdateOpts();
        }
        (void)SetCurLogType(logh, T_REQUEST);

        if (!gatekeeper_test && !debug) {
          pid = fork();
          if (pid) {
            LOGM(VARP, logh, LEV_INFO, T_PRE, "Starting Executor with pid = %d", pid);
            sock.CloseListened();
          }
        }

        if (!pid) {
          bool value = false;

          if (!debug && !gatekeeper_test)
            sock.CloseListener();
          if (sock.AcceptGSIAuthentication()) {

            LOGM(VARP, logh, LEV_INFO, T_PRE, "Self    : %s", sock.own_subject.c_str());
            LOGM(VARP, logh, LEV_INFO, T_PRE, "Self CA : %s", sock.own_ca.c_str());

            std::string user    = sock.peer_subject;
            std::string userca  = sock.peer_ca;
            subject = sock.own_subject;
            ca = sock.own_ca;

            LOGM(VARP, logh, LEV_INFO, T_PRE, "At: %s Received Contact from:", timestamp().c_str());
            LOGM(VARP, logh, LEV_INFO, T_PRE, " user: %s", user.c_str());
            LOGM(VARP, logh, LEV_INFO, T_PRE, " ca  : %s", userca.c_str());
            LOGM(VARP, logh, LEV_INFO, T_PRE, " serial: %s", sock.peer_serial.c_str());

            LOG(logh, LEV_DEBUG, T_PRE, "Starting Execution.");
            value = Execute(sock.own_key, sock.own_cert, sock.peer_cert, sock.GetContext());
          }
          else {
            LOGM(VARP, logh, LEV_INFO, T_PRE, "Failed to authenticate peer");
            sock.CleanSocket();
          }

          if (!debug && !gatekeeper_test) {
            sock.Close();
            exit(value == false ? 1 : 0);
          }
          else {
            sock.CloseListened();
          }
        }
      }
      else {
        LOGM(VARP, logh, LEV_ERROR, T_PRE, "Cannot listen on port %d", daemon_port);
        exit(1);
      }
    }
  }
  catch (...) {}
}

bool
VOMSServer::Execute(EVP_PKEY *key, X509 *issuer, X509 *holder, gss_ctx_id_t context)
{
  std::string message;

  if (!sock.Receive(message)) {
    LOG(logh, LEV_ERROR, T_PRE, "Unable to receive request.");
    sock.CleanSocket();
    return false;
  }

  if (message == "0") {
    /* GSI Clients may send a "0" first (spurious) message. Just ignore it. */
    if (!sock.Receive(message)) {
      LOG(logh, LEV_ERROR, T_PRE, "Unable to receive request.");
      sock.CleanSocket();
      return false;
    }
  }

  LOGM(VARP, logh, LEV_DEBUG, T_PRE, "Received Request: %s", message.c_str());

  struct request r;

  if (!XML_Req_Decode(message, r)) {
    LOGM(VARP, logh, LEV_ERROR, T_PRE, "Unable to interpret command: %s",message.c_str());
    return false;
  }

  std::vector<std::string> comm = r.command;

  bool dobase64 = base64encoding | r.base64;
  int requested = r.lifetime;

  std::vector<std::string> targs;

  firstfqan = "";
  ordering.clear();

  parse_order(r.order, ordering);
  parse_targets(r.targets, targs);

  std::vector<gattrib> attributes;
  std::string data = "";
  std::string tmp="";
  bool result = true;
  bool result2 = true;
  std::vector<errorp> errs;
  errorp err;

  /* Interpret user requests */

  if (requested != 0) {
    if (requested == -1)
      requested = validity;
    else if (validity < requested) {
      err.num = WARN_SHORT_VALIDITY;
      err.message = uri + ": The validity of this VOMS AC in your proxy is shortened to " +
        stringify(validity, tmp) + " seconds!";
      errs.push_back(err);
      requested = validity;
    }
  }

  std::string command;

  std::vector<std::string> fqans;
  std::vector<gattrib> attribs;
  signed long int uid = -1;

  sqliface::interface *newdb = db->getSession();

  if (!newdb->operation(OPERATION_GET_USER, &uid, holder)) {
    LOG(logh, LEV_ERROR, T_PRE, "Error in executing request!");
    LOG(logh, LEV_ERROR, T_PRE, newdb->errorMessage());
    db->releaseSession(newdb);
    err.num = ERR_NOT_MEMBER;
    if (command == (std::string("G/")+ voname))
      err.message = voname + ": User unknown to this VO.";
    else
      err.message = voname + ": Unable to satisfy " + command + " Request!";

    LOG(logh, LEV_ERROR, T_PRE, err.message.c_str());
    errs.push_back(err);
    std::string ret = XML_Ans_Encode("A", errs, dobase64);
    LOGM(VARP, logh, LEV_DEBUG, T_PRE, "Sending: %s", ret.c_str());
    sock.Send(ret);
    return false;

  }

  LOGM(VARP, logh, LEV_ERROR, T_PRE, "Userid = \"%ld\"", uid);

  for(std::vector<std::string>::iterator i = comm.begin(); i < comm.end(); ++i)
  {
    char comm = '\0';
    char *group = NULL;
    char *role = NULL;
    bool valid = determine_group_and_role(*i, &comm, &group, &role);

    LOGM(VARP, logh, LEV_INFO, T_PRE, "Next command : %s", i->c_str());

    if (valid) {

      /* Interpret request by first character */
      switch (comm) {
      case 'A':
        if (result = newdb->operation(OPERATION_GET_ALL, &fqans, uid))
          result2 = newdb->operation(OPERATION_GET_ALL_ATTRIBS, &attribs, uid);
        break;

      case 'R':
        if (result = newdb->operation(OPERATION_GET_ROLE, &fqans, uid, role))
          result2 = newdb->operation(OPERATION_GET_ROLE_ATTRIBS, &attribs, uid, role);
        break;

      case 'G':
        if (result = newdb->operation(OPERATION_GET_GROUPS, &fqans, uid))
          result2 = newdb->operation(OPERATION_GET_GROUPS_ATTRIBS, &attribs, uid);
        break;

      case 'B':
        if (result = newdb->operation(OPERATION_GET_GROUPS_AND_ROLE, &fqans, uid, group, role))
          result2 = newdb->operation(OPERATION_GET_GROUPS_AND_ROLE_ATTRIBS, &attribs, uid, group, role);
        break;

      case 'N':
        result = newdb->operation(OPERATION_GET_ALL, &fqans, uid);
        break;

      default:
        result = false;
        LOGM(VARP, logh, LEV_ERROR, T_PRE, "Unknown Command \"%c\"", comm);
        break;
      }
    }
    else
      result = false;

    free(group); // role is automatically freed.

    if(!result) {
      LOGM(VARP, logh, LEV_DEBUG, T_PRE, "While retrieving fqans: %s", newdb->errorMessage());
    }
      break;

    if (!result2)
      LOGM(VARP, logh, LEV_DEBUG, T_PRE, "While retrieving attributes: %s", newdb->errorMessage());

  }
  db->releaseSession(newdb);

  // remove duplicates
  std::sort(fqans.begin(), fqans.end());
  fqans.erase(std::unique(fqans.begin(), fqans.end()), fqans.end());

  // remove duplicates from attributes
  std::sort(attribs.begin(), attribs.end());
  attribs.erase(std::unique(attribs.begin(), attribs.end()), 
                attribs.end());

  if(result && !fqans.empty()) {
    orderattribs(fqans);
  }

  if (!result) {
    LOG(logh, LEV_ERROR, T_PRE, "Error in executing request!");
    err.num = ERR_NOT_MEMBER;
    if (command == (std::string("G/")+ voname))
      err.message = voname + ": User unknown to this VO.";
    else
      err.message = voname + ": Unable to satisfy " + command + " Request!";

    LOG(logh, LEV_ERROR, T_PRE, err.message.c_str());
    errs.push_back(err);
    std::string ret = XML_Ans_Encode("A", errs, dobase64);
    LOGM(VARP, logh, LEV_DEBUG, T_PRE, "Sending: %s", ret.c_str());
    sock.Send(ret);
    return false;
  }

  if(!fqans.empty()) {
    /* check whether the user is allowed to requests those attributes */
    vomsdata v("", "");
    v.SetVerificationType((verify_type)(VERIFY_SIGN));
    v.RetrieveFromCtx(context, RECURSE_DEEP);

    /* find the attributes corresponding to the vo */
    std::vector<std::string> existing;
    for(std::vector<voms>::iterator index = (v.data).begin(); index != (v.data).end(); ++index) {
      if(index->voname == voname)
        existing.insert(existing.end(),
                     index->fqan.begin(),
                     index->fqan.end());
    }

    /* if attributes were found, only release an intersection beetween the requested and the owned */
    std::vector<std::string>::iterator end = fqans.end();
    bool subset = false;
    if (!existing.empty())
      if ((fqans.erase(remove_if(fqans.begin(),
                                 fqans.end(),
                                 bind2nd(std::ptr_fun(not_in), existing)),
                       fqans.end()) != end))
        subset = true;

    // no attributes can be send
    if (fqans.empty()) {
      LOG(logh, LEV_ERROR, T_PRE, "Error in executing request!");
      err.num = ERR_ATTR_EMPTY;
      err.message = voname + " : your certificate already contains attributes, only a subset of them can be issued.";
      errs.push_back(err);
      std::string ret = XML_Ans_Encode("A", errs, dobase64);
      LOGM(VARP, logh, LEV_DEBUG, T_PRE, "Sending: %s", ret.c_str());
      sock.Send(ret);
      return false;
    }

    // some attributes can't be send
    if(subset) {
      LOG(logh, LEV_ERROR, T_PRE, "Error in executing request!");
      err.num = WARN_ATTR_SUBSET;
      err.message = voname + " : your certificate already contains attributes, only a subset of them can be issued.";
      errs.push_back(err);
    }
  }

  if (!fqans.empty()) {
    if (!firstfqan.empty()) {
      std::vector<std::string>::iterator i = fqans.begin();
      if (i != fqans.end()) {
        LOGM(VARP, logh, LEV_DEBUG, T_PRE,  "fq = %s", firstfqan.c_str());
        if (*i != firstfqan) {
          err.num = WARN_NO_FIRST_SELECT;
          err.message = "FQAN: " + *i + " is not the first selected!\n";
          errs.push_back(err);
        }
      }
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    // test logging retrieved attributes

    if(result && !attributes.empty()) {
      for(std::vector<gattrib>::iterator i = attributes.begin(); i != attributes.end(); ++i)
        LOGM(VARP, logh, LEV_DEBUG, T_PRE,  "User got attributes: %s", i->str().c_str());
    }
    else
      LOGM(VARP, logh, LEV_DEBUG, T_PRE,  "User got no attributes or something went wrong searching for them.");

    // convert to string
    std::vector<std::string> attributes_compact;
    for(std::vector<gattrib>::iterator i = attribs.begin(); i != attribs.end(); ++i)
      attributes_compact.push_back(i->str());


    ///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    BIGNUM * serial = get_serial();

    int res = 1;
    std::string codedac;

    if (comm[0] != "N") {
      if (!serial)
        LOG(logh, LEV_ERROR, T_PRE, "Can't get Serial Number!");

      if (serial) {
        AC *a = AC_new();

        LOGM(VARP, logh, LEV_DEBUG, T_PRE, "length = %d", i2d_AC(a, NULL));
        if (a)
          res = createac(issuer, sock.own_stack, holder, key, serial,
                         fqans, targs, attributes_compact, &a, voname, uri, requested, !newformat);

        LOGM(VARP, logh, LEV_DEBUG, T_PRE, "length = %d", i2d_AC(a, NULL));
        //        BN_free(serial);

        if (!res) {
          unsigned int len = i2d_AC(a, NULL);

          unsigned char *tmp = (unsigned char *)OPENSSL_malloc(len);
          unsigned char *ttmp = tmp;

          LOGM(VARP, logh, LEV_DEBUG, T_PRE, "length = %d", len);

          if (tmp) {
            i2d_AC(a, &tmp);
            codedac = std::string((char *)ttmp, len);
          }
          free(ttmp);
        }
        else {
          err.num = ERR_NOT_MEMBER;
          err.message = std::string(get_error(res));
          errs.push_back(err);
        }
        AC_free(a);
      }

      if (res || codedac.empty()) {
        LOG(logh, LEV_ERROR, T_PRE, "Error in executing request!");
        err.message = voname + ": Unable to satisfy " + command + " request due to database error.";
        errs.push_back(err);
        std::string ret = XML_Ans_Encode("A", errs, dobase64);
        LOGM(VARP, logh, LEV_DEBUG, T_PRE, "Sending: %s", ret.c_str());
        sock.Send(ret);

        return false;
      }
    }

    (void)SetCurLogType(logh, T_RESULT);

    if (comm[0] == "N")
      data = "";

    for (std::vector<std::string>::iterator i = fqans.begin(); i != fqans.end(); i++) {
      LOGM(VARP, logh, LEV_INFO, T_PRE, "Request Result: %s",  (*i).c_str());
      if (comm[0] == "N")
        data += (*i).c_str() + std::string("\n");
    }

    std::string ret = XML_Ans_Encode(codedac, data, errs, dobase64);

    LOGM(VARP, logh, LEV_DEBUG, T_PRE, "OUTPUT: %s", ret.c_str());
    sock.Send(ret);
  }
  else if (!data.empty()) {
    std::string ret = XML_Ans_Encode("", data, errs, dobase64);
    LOGM(VARP, logh, LEV_DEBUG, T_PRE, "OUTPUT: %s", ret.c_str());
    sock.Send(ret);
  }
  else {
    err.num = ERR_NOT_MEMBER;
    err.message = std::string("You are not a member of the ") + voname + " VO!";
    errs.push_back(err);
    std::string ret = XML_Ans_Encode("", errs, dobase64);
    sock.Send(ret);
  }

  return true;
}

void VOMSServer::UpdateOpts(void)
{
  std::string nlogfile = logfile;
  std::string fakeuri = "";
  int nblog = 50;
  bool progversion = false;
  int nport;

  struct option opts[] = {
    {"test",            0, (int *)&gatekeeper_test,   OPT_BOOL},
    {"conf",            1, NULL,                      OPT_CONFIG},
    {"port",            1, &nport,                    OPT_NUM},
    {"logfile",         1, (int *)&nlogfile,          OPT_STRING},
    {"globusid",        1, (int *)&globusid,          OPT_STRING},
    {"globuspwd",       1, (int *)&globuspwd,         OPT_STRING},
    {"x509_cert_dir",   1, (int *)&x509_cert_dir,     OPT_STRING},
    {"x509_cert_file",  1, (int *)&x509_cert_file,    OPT_STRING},
    {"x509_user_proxy", 1, (int *)&x509_user_proxy,   OPT_STRING},
    {"x509_user_cert",  1, (int *)&x509_user_cert,    OPT_STRING},
    {"x509_user_key",   1, (int *)&x509_user_key,     OPT_STRING},
    {"desired_name",    1, (int *)&desired_name_char, OPT_STRING},
    {"foreground",      0, (int *)&foreground,        OPT_BOOL},
    {"username",        1, (int *)&username,          OPT_STRING},
    {"timeout",         1, &validity,                 OPT_NUM},
    {"dbname",          1, (int *)&dbname,            OPT_STRING},
    {"contactstring",   1, (int *)&contactstring,     OPT_STRING},
    {"mysql-port",      1, (int *)&mysql_port,        OPT_NUM},
    {"mysql-socket",    1, (int *)&mysql_socket,      OPT_STRING},
    {"passfile",        1, (int *)&passfile,          OPT_STRING},
    {"vo",              1, (int *)&voname,            OPT_STRING},
    {"uri",             1, (int *)&fakeuri,           OPT_STRING},
    {"globus",          1, &version,                  OPT_NUM},
    {"version",         0, (int *)&progversion,       OPT_BOOL},
    {"backlog",         1, &nblog,                    OPT_NUM},
    {"debug",           0, (int *)&debug,             OPT_BOOL},
    {"code",            1, &code,                     OPT_NUM},
    {"loglevel",        1, &loglev,                   OPT_NUM},
    {"logtype",         1, &logt,                     OPT_NUM},
    {"logformat",       1, (int *)&logf,              OPT_STRING},
    {"logdateformat",   1, (int *)&logdf,             OPT_STRING},
    {"sqlloc",          1, (int *)&sqllib,            OPT_STRING},
    {"compat",          0, (int *)&compat_flag,       OPT_BOOL},
    {"socktimeout",     1, &socktimeout,              OPT_NUM},
    {"logmax",          1, &logmax,                   OPT_NUM},
    {"newformat",       0, (int *)&newformat,         OPT_BOOL},
    {"skipcacheck",     0, (int *)&insecure,          OPT_BOOL},
    {"shortfqans",      0, (int *)&shortfqans,        OPT_BOOL},
    {0, 0, 0, 0}
  };

  (void)SetCurLogType(logh, T_STARTUP);

  nlogfile = "";

  if (!getopts(ac, av, opts)) {
    LOG(logh, LEV_ERROR, T_PRE, "Unable to read options!");
    throw VOMSInitException("unable to read options");
  }

  short_flags = shortfqans;

  if (nlogfile.size() != 0) {
    LOGM(VARP, logh, LEV_INFO, T_PRE, "Attempt redirecting logs to: %s", logfile.c_str());

    LogOption(logh, "NAME", nlogfile.c_str());

    logfile = nlogfile;
  }

  LogOptionInt(logh, "MAXSIZE", logmax);
  LogOption(logh, "DATEFORMAT", logdf.c_str());

  sock.SetFlags(GSS_C_MUTUAL_FLAG | GSS_C_CONF_FLAG | GSS_C_INTEG_FLAG);

  if (logh) {
    loglevels lev;

    switch(loglev) {
    case 1: lev = LEV_NONE; break;
    case 2: lev = LEV_ERROR; break;
    case 3: lev = LEV_WARN; break;
    case 4: lev = LEV_INFO; break;
    case 5: lev = LEV_DEBUG; break;
    default: lev = LEV_DEBUG; break;
    }
    if (debug)
      lev = LEV_DEBUG;
    (void)LogLevel(logh, lev);

    if (lev == LEV_DEBUG)
      logt = T_STARTUP|T_REQUEST|T_RESULT;


    (void)LogType(logh, logt);
    (void)SetCurLogType(logh, T_STARTUP);
    (void)LogService(logh, "vomsd");
    (void)LogFormat(logh, logf.c_str());
    //    (void)LogDateFormat(logh, logdf.c_str());
  }

  if (nport != daemon_port) {
    if (!sock.ReOpen(daemon_port = nport, version, nblog, true))
      LOG(logh, LEV_ERROR, T_PRE, "Failed to reopen socket! Server in unconsistent state.");
  }
  else if (nblog != backlog)
    sock.AdjustBacklog(backlog = nblog);

  if (fakeuri.empty()) {
    int   ok;

    int   hostnamesize = 50;
    char *hostname = new char[1];
    do {
      delete[] hostname;
      hostname = new char[hostnamesize];
      ok = gethostname(hostname, hostnamesize);
      hostnamesize += 50;
    } while (ok);
    std::string temp;

    uri = std::string(hostname) + ":" + stringify(daemon_port, temp);
    delete[] hostname;
  }
  else
    uri = fakeuri;

  setenv("GLOBUSID", globusid.c_str(), 1);

  if (!getpasswd(passfile, logh)){
    throw VOMSInitException("can't read password file!");
  }

  if (!globuspwd.empty()) {
    setenv("GLOBUSPWD", globuspwd.c_str(), 1);
  }

  if (!x509_cert_dir.empty()) {
    setenv("X509_CERT_DIR", x509_cert_dir.c_str(), 1);
  }
  if (!x509_cert_file.empty()) {
    setenv("X509_CERT_FILE", x509_cert_file.c_str(), 1);
  }
  if (!x509_user_proxy.empty()) {
    setenv("X509_USER_PROXY", x509_user_proxy.c_str(), 1);
  }
  if (!x509_user_cert.empty()) {
    setenv("X509_USER_CERT", x509_user_cert.c_str(), 1);
  }
  if (!x509_user_key.empty()) {
    setenv("X509_USER_KEY", x509_user_key.c_str(), 1);
  }

  if (debug)
    LOG(logh, LEV_INFO, T_PRE, "DEBUG MODE ACTIVE ");
  else
    LOG(logh, LEV_INFO, T_PRE, "DEBUG MODE INACTIVE ");
}

static BIGNUM *get_serial()
{
  unsigned char uuid[16];
  initialize_uuid_generator();
  generate_uuid(uuid);
  BIGNUM *number = NULL;

  return BN_bin2bn(uuid, 16, number);
}

static bool determine_group_and_role(std::string command, char *comm, char **group,
                                     char **role)
{
  *role = *group = NULL;

  if (command.empty())
    return false;

  char *string = strdup(command.c_str()+1);

  if (string[0] != '/') {
    /* old syntax, or maybe new? */
    *comm = command[0];

    *group = string;

    switch (*comm) {
    case 'G':
      *role = NULL;
      break;
    case 'R':
      *role = string;
      break;
    case 'B':
      *role = strchr(string, ':');
      if (*role)
        *role++ = '\0';
    }
  }
  else {
    /* fqan syntax */
    char *divider  = strstr(string, "/Role=");
    char *divider2 = strstr(string, ":");
    if (divider) {
      if (divider == string) {
        *group = string;
        *role = divider + 6;
        *comm = 'R';
      }
      else {
        *group = string;
        *role = divider + 6;
        *divider='\0';
        *comm='B';
      }
    }
    else if (divider2) {
      if (divider2 == string) {
        *group = string;
        *role = divider2+1;
        *comm = 'R';
      }
      else {
        *group = string;
        *role = divider2+1;
        *divider2 = '\0';
        *comm = 'B';
      }
    }
    else {
      *group = string;
      *role = NULL;
      *comm='G';
    }
    if (strcmp(*group, "/") == 0) {
      free(string);
      *role = *group = NULL;
      *comm = 'A';
    }
    if (strcmp(*group, "//") == 0) {
      free(string);
      *role = *group = NULL;
      *comm='N';
    }
  }

  if (!acceptable(*group) || !acceptable(*role)) {
    free(string);
    *role = *group = NULL;
    return false;
  }

  return true;
}

