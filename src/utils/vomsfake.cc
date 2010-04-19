/*********************************************************************
 *
 * Authors: Vincenzo Ciaschini - Vincenzo.Ciaschini@cnaf.infn.it 
 *          Valerio Venturi - Valerio.Venturi@cnaf.infn.it 
 *
 * Copyright (c) Members of the EGEE Collaboration. 2004-2010.
 * See http://www.eu-egee.org/partners/ for details on the copyright holders.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Parts of this code may be based upon or even include verbatim pieces,
 * originally written by other people, in which case the original header
 * follows.
 *
 *********************************************************************/

#include "config.h"
#include "replace.h"

#include "options.h"
#include "data.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>

#include <voms_api_nog.h>

extern "C" {
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>

#include "listfunc.h"
#include "credentials.h"
#include "parsertypes.h"
#include "vomsparser.h"
#include "vomsproxy.h"

VOLIST *volist = NULL;
  extern int yyparse();
  extern FILE *yyin;
}

#include <voms_api_nog.h>

#include "vomsfake.h"
#include "ccwrite.h"

extern "C" {

#include "myproxycertinfo.h"
extern int writeac(const X509 *issuerc, const STACK_OF(X509) *certstack, const X509 *holder, 
		   const EVP_PKEY *pkey, BIGNUM *s, char **c, 
		   const char *t, char **attributes, AC **ac, const char *voname, 
       const char *uri, int valid, int old, int startpast);
}

static int time_to_sec(std::string timestring);
static long mystrtol(char *number, int limit);

extern int AC_Init();

#include "init.h"

const std::string SUBPACKAGE      = "voms-proxy-fake";

/* use name specific to each distribution (defined in configure.in) */

const std::string location = (getenv(LOCATION_ENV) ? getenv(LOCATION_ENV) : LOCATION_DIR);
const std::string CONFILENAME     = (location + "/etc/vomses");
const std::string USERCONFILENAME = std::string(USER_DIR) + std::string("/vomses");

/* global variable for output control */

bool debug = false;
bool quiet = false;

extern "C" {
  
static int (*pw_cb)() = NULL;

static int pwstdin_callback(char * buf, int num, UNUSED(int w)) 
{
  int i;
  
  if (!(fgets(buf, num, stdin))) {
    std::cerr << "Failed to read pass-phrase from stdin" << std::endl;
    return -1;
  }

  i = strlen(buf);
  if (buf[i-1] == '\n') {
      buf[i-1] = '\0';
      i--;
  }
  return i;
}
  
static int kpcallback(int p, int UNUSED(n)) 
{
  char c='B';
    
  if (quiet) return 0;
    
  if (p == 0) c='.';
  if (p == 1) c='+';
  if (p == 2) c='*';
  if (p == 3) c='\n';
  if (!debug) c = '.';
  fputc(c,stderr);

  return 0;
}
  
extern int proxy_verify_cert_chain(X509 * ucert, STACK_OF(X509) * cert_chain, proxy_verify_desc * pvd);
extern void proxy_verify_ctx_init(proxy_verify_ctx_desc * pvxd);
  
}
std::vector<std::string> targets;


int main(int argc, char** argv) 
{
  struct rlimit newlimit = {0,0};

  if (setrlimit(RLIMIT_CORE, &newlimit) != 0)
    exit(1);

  if (AC_Init()) {
    InitProxyCertInfoExtension(1);
    Fake v(argc, argv);
    v.Run();

    return 0;
  }
  return 1;
}

extern int yydebug;
Fake::Fake(int argc, char ** argv) :   confile(CONFILENAME), 
                                       separate(""), uri(""),bits(1024),
                                       hours(12), limit_proxy(false),
                                       vomslife(-1), proxyver(0),
                                       pathlength(1), verify(false), 
                                       noregen(false), version(0),
#ifdef CLASS_ADD
                                       class_add_buf(NULL),
                                       class_add_buf_len(0),
#endif					   
                                       ucert(NULL), upkey(NULL), cert_chain(NULL),
                                       aclist(NULL), voID(""),
                                       hostcert(""), hostkey(""),
                                       newformat(false), newsubject(""),
                                       rfc(false), pastac("0"), pastproxy("0")
{
  
  bool progversion = false;
  std::string crtdir;
  std::string crtfile;
  std::string kfile;
  std::string ofile;
  std::vector<std::string> order;
  bool pwstdin = false;

  yydebug = 0;

  if (strrchr(argv[0],'/'))
    program = strrchr(argv[0],'/') + 1;
  else
    program = argv[0];
  
  /* usage message */

  static std::string LONG_USAGE =		\
    "\n" \
    "    Options\n" \
    "    -help, -usage                  Displays usage\n" \
    "    -version                       Displays version\n" \
    "    -debug                         Enables extra debug output\n" \
    "    -quiet, -q                     Quiet mode, minimal output\n" \
    "    -verify                        Verifies certificate to make proxy for\n" \
    "    -pwstdin                       Allows passphrase from stdin\n" \
    "    -limited                       Creates a limited proxy\n" \
    "    -hours H                       Proxy is valid for H hours (default:12)\n" \
    "    -bits                          Number of bits in key {512|1024|2048|4096} (default:1024)\n" \
    "    -cert     <certfile>           Non-standard location of user certificate\n" \
    "    -key      <keyfile>            Non-standard location of user key\n" \
    "    -certdir  <certdir>            Non-standard location of trusted cert dir\n" \
    "    -out      <proxyfile>          Non-standard location of new proxy cert\n" \
    "    -voms <voms>                   Specify voms server. :command is optional.\n" \
    "    -uri <uri>                     Specifies the <hostname>:<port> of the fake server.\n" \
    "    -target <hostname>             Targets the AC against a specific hostname.\n" \
    "    -vomslife <H>                  Try to get a VOMS pseudocert valid for H hours.\n" \
    "    -voinfo <file>                 Gets AC information from <file>\n" \
    "    -include <file>                Include the contents of the specified file.\n" \
    "    -conf <file>                   Read options from <file>.\n" \
    "    -policy <policyfile>           File containing policy to store in the ProxyCertInfo extension.\n" \
    "    -pl, -policy-language <oid>    OID string for the policy language.\n" \
    "    -path-length <l>               Allow a chain of at most l proxies to be generated from this ones.\n" \
    "    -globus                        Globus version.\n" \
    "    -proxyver <n>                  Version of proxy certificate.\n" \
    "    -rfc                           Create RFC-conforming proxies (synonim of --proxyver 4)\n"             
    "    -noregen                       Doesn't regenerate a new proxy for the connection.\n" \
    "    -separate <file>               Saves the informations returned by the server on file <file>.\n" \
    "    -hostcert <file>               Fake host certificate.\n" \
    "    -hostkey <file>                Fake host private key.\n" \
    "    -fqan <string>                 String to include in the AC as the granted FQAN.\n" \
    "    -newformat                     Creates ACs according to the new format.\n" \
    "    -newsubject <string>           Subject of the new certificate.\n" \
    "    -pastac <seconds>\n"
    "    -pastac <hour:minutes>         Start the validity of the AC in the past,\n"\
    "    -pastproxy <seconds>\n"
    "    -pastproxy <hour:minutes>      Start the validity of the proxy in the past,\n"\
    "\n";

  set_usage(LONG_USAGE);

  /* parse command-line option */

  std::string voinfo;

  struct option opts[] = {
    {"help",            0, NULL,                OPT_HELP},
    {"usage",           0, NULL,                OPT_HELP},
    {"version",         0, (int *)&progversion, OPT_BOOL},
    {"cert",            1, (int *)&crtfile,     OPT_STRING},
    {"certdir",         1, (int *)&crtdir,      OPT_STRING},
    {"out",             1, (int *)&ofile,       OPT_STRING},
    {"key",             1, (int *)&kfile,       OPT_STRING},
    {"include",         1, (int *)&incfile,     OPT_STRING},
    {"hours",           1,        &hours,       OPT_NUM},
    {"vomslife",        1,        &vomslife,    OPT_NUM},
    {"bits",            1,        &bits,        OPT_NUM},
    {"debug",           0, (int *)&debug,       OPT_BOOL},
    {"limited",         0, (int *)&limit_proxy, OPT_BOOL},
    {"verify",          0, (int *)&verify,      OPT_BOOL},
    {"q",               0, (int *)&quiet,       OPT_BOOL},
    {"quiet",           0, (int *)&quiet,       OPT_BOOL},
    {"pwstdin",         0, (int *)&pwstdin,     OPT_BOOL},
    {"conf",            1, NULL,                OPT_CONFIG},
    {"voms",            1, (int *)&voms,        OPT_STRING},
    {"target",          1, (int *)&targets,     OPT_MULTI},
    {"globus",          1,        &version,     OPT_NUM},
    {"proxyver",        1,        &proxyver,    OPT_NUM},
    {"rfc",             0, (int *)&rfc,         OPT_BOOL},
    {"policy",          1, (int *)&policyfile,  OPT_STRING},
    {"policy-language", 1, (int *)&policylang,  OPT_STRING},
    {"pl",              1, (int *)&policylang,  OPT_STRING},
    {"path-length",     1,        &pathlength,  OPT_NUM},
    {"separate",        1, (int *)&separate,    OPT_STRING},
    {"uri",             1, (int *)&uri,         OPT_STRING},
    {"hostcert",        1, (int *)&hostcert,    OPT_STRING},
    {"hostkey",         1, (int *)&hostkey,     OPT_STRING},
    {"fqan",            1, (int *)&fqans,       OPT_MULTI},
    {"newformat",       1, (int *)&newformat,   OPT_BOOL},
    {"newsubject",      1, (int *)&newsubject,  OPT_STRING},
    {"voinfo",          1, (int *)&voinfo,      OPT_STRING},
    {"pastac",          1, (int *)&pastac,      OPT_STRING},
    {"pastproxy",       1, (int *)&pastproxy,   OPT_STRING},
#ifdef CLASS_ADD
    {"classadd",        1, (int *)class_add_buf,OPT_STRING},
#endif
    {0, 0, 0, 0}
  };

  if (!getopts(argc, argv, opts))
    exit(1);
  
  if(debug) {
    quiet = false;
    yydebug = 1;
  }
  
  if (!voinfo.empty()) {
    FILE *file = fopen(voinfo.c_str(), "rb");
    if (file) {
      yyin = file;
      if (yyparse()) {
        Print(ERROR) << "Error: Cannot parse voinfo file: " << voinfo << std::endl;
        exit(1);
      }
    }
    else {
      Print(ERROR) << "Error opening voinfo file: " << voinfo << std::endl;
      exit(1);
    }
  }

  /* show version and exit */
  
  if (progversion) {
    Print(FORCED) << SUBPACKAGE << "\nVersion: " << VERSION << std::endl;
    Print(FORCED) << "Compiled: " << __DATE__ << " " << __TIME__ << std::endl;
    exit(0);
  }

  /* get vo */
  
  char *vo = getenv("VO");
  if (vo != NULL && strcmp(vo, "") != 0)
    voID = vo;
  
  /* certficate duration option */
  
  if (vomslife == -1)
    vomslife = hours;
  
  VO *voelem = NULL;

  /* collect local vo information */
  if (!voms.empty()) {
    if (!volist) {
      volist = (VOLIST *)calloc(1, sizeof(VOLIST));
      volist->vos = NULL;
    }
    voelem = (VO*)calloc(1, sizeof(VO));
    volist->vos = (VO**)listadd((char**)volist->vos, (char*)voelem, sizeof(VO*));

    voelem->hostcert = (char*)hostcert.c_str();
    voelem->hostkey = (char*)hostkey.c_str();
    voelem->uri = (char*)uri.c_str();
    voelem->voname = (char*)voms.c_str();
    voelem->vomslife = vomslife;
    voelem->pastac = (char*)pastac.c_str();

    voelem->fqans = (char **)malloc(sizeof(char*)*(fqans.size()+1));
    for (unsigned int i  = 0; i < fqans.size(); i++)
      voelem->fqans[i] = (char*)(fqans[i].c_str());
    voelem->fqans[fqans.size()] = NULL;
    
    std::string targ;
    for (unsigned int i  = 0; i < targets.size(); i++)
      targ += targets[i];
    voelem->targets = (char*)(targ.c_str());
  }
  
  /* A failure here exits the program entirely */
  VerifyOptions();

  /* allow password from stdin */

  if(pwstdin)
    pw_cb = (int (*)())(pwstdin_callback);

  /* with --debug prints configuration files used */

  Print(DEBUG) << "Using configuration directory " << confile << std::endl;

  /* file used */
  
  cacertfile = NULL;
  certdir  = (crtdir.empty()  ? NULL : const_cast<char *>(crtdir.c_str()));
  outfile  = (ofile.empty()   ? NULL : const_cast<char *>(ofile.c_str()));
  certfile = (crtfile.empty() ? NULL : const_cast<char *>(crtfile.c_str()));
  keyfile  = (kfile.empty()   ? NULL : const_cast<char *>(kfile.c_str()));

  /* prepare proxy_cred_desc */

  if(!pcdInit())
    exit(3);

}

Fake::~Fake() 
{
  free(cacertfile);
  free(certdir);
  free(certfile);
  free(keyfile);
  free(outfile);

  OBJ_cleanup();
}

bool Fake::Run() 
{
  /* set output file and environment */
  
  char * oldenv = getenv("X509_USER_PROXY");
  
  if(!noregen) {
    std::stringstream tmpproxyname;
    tmpproxyname << "/tmp/tmp_x509up_u" << getuid() << "_" << getpid();
    proxyfile = tmpproxyname.str();
    setenv("X509_USER_PROXY", proxyfile.c_str(), 1);
  }
  
  /* contacts servers for each vo */

  if (volist)
    if (!Retrieve(volist))
      exit(1);

  /* set output file and environment */
  
  proxyfile = outfile;
  setenv("X509_USER_PROXY", proxyfile.c_str(), 1);  
  
  /* with separate write info to file and exit */
  
  if (!separate.empty() && aclist) {
    if(!WriteSeparate())
      Print(WARN) << "Wasn't able to write to " << separate << std::endl;
    exit(0);
  }
  
  /* create a proxy containing the data retrieved from VOMS servers */
  
  Print(INFO) << "Creating proxy " << std::flush; 
  Print(DEBUG) << "to " << proxyfile << " " << std::flush;
  if(CreateProxy("", aclist, proxyver)) {
    listfree((char **)aclist, (freefn)AC_free);
    goto err;
  }
  else
    free(aclist);
  
  /* unset environment */
  
  if (!oldenv)
    unsetenv("X509_USER_PROXY");
  else {
    setenv("X509_USER_PROXY", oldenv, 1);
  }
  
  /* assure user certificate is not expired or going to, else ad but still create proxy */
  
  Test();
  
  return true;

 err:
  
  Error();

  return false;

}

bool Fake::CreateProxy(std::string data, AC ** aclist, int version) 
{
  struct VOMSProxyArguments *args = VOMS_MakeProxyArguments();
  int ret = -1;

  if (args) {
    args->proxyfilename = strdup(proxyfile.c_str());
    if (!incfile.empty())
      args->filename      = strdup(incfile.c_str());
    args->aclist        = aclist;
    args->proxyversion  = version;
    if (!data.empty()) {
      args->data          = (char*)data.data();
      args->datalen       = data.length();
    }
    if (!newsubject.empty()) {
      args->newsubject       = strdup(newsubject.c_str());
      args->newsubjectlen    = strlen(args->newsubject);
    }
    args->cert          = ucert;
    args->chain         = cert_chain;
    args->key           = upkey;
    args->bits          = bits;
    if (!policyfile.empty())
      args->policyfile    = strdup(policyfile.c_str());
    if (!policylang.empty())
      args->policylang    = strdup(policylang.c_str());
    args->pathlength    = pathlength;
    args->hours         = hours;
    args->minutes       = 0;
    args->limited       = limit_proxy;
    args->voID          = strdup(voID.c_str());
    args->callback      = (int (*)())kpcallback;
    args->pastproxy     = time_to_sec(pastproxy);

    if (args->pastproxy == -1) {
      Print(ERROR) << "Minutes and seconds should be < 59 and >= 0" << std::endl;
      exit(1);
    }

    int warn = 0;
    void *additional = NULL;

    struct VOMSProxy *proxy = VOMS_MakeProxy(args, &warn, &additional);

    if (proxy)
      ret = VOMS_WriteProxy(proxyfile.c_str(), proxy);

    VOMS_FreeProxy(proxy);
    VOMS_FreeProxyArguments(args);

    Print(INFO) << " Done" << std::endl << std::flush;
  }

  return ret == -1;
}

bool Fake::WriteSeparate() 
{
  if (aclist) {
    BIO * out = BIO_new(BIO_s_file());
    BIO_write_filename(out, (char *)separate.c_str());
    
    while(*aclist)
#ifdef TYPEDEF_I2D_OF
      if (!PEM_ASN1_write_bio(((i2d_of_void*)i2d_AC), "ATTRIBUTE CERTIFICATE", out, (char *)*(aclist++), NULL, NULL, 0, NULL, NULL))
#else
      if (!PEM_ASN1_write_bio(((int (*)())i2d_AC), "ATTRIBUTE CERTIFICATE", out, (char *)*(aclist++), NULL, NULL, 0, NULL, NULL))
#endif
      {
        Print(ERROR) << "Unable to write to file" << std::endl;
        return false;
      }
    
    BIO_free(out);
  
    Print(INFO) << "Wrote ACs to " << separate << std::endl;
  }

  return true;
}

void Fake::Test() 
{
  ASN1_UTCTIME * asn1_time = ASN1_UTCTIME_new();
  X509_gmtime_adj(asn1_time, 0);
  time_t time_now = ASN1_UTCTIME_mktime(asn1_time);
  time_t time_after = ASN1_UTCTIME_mktime(X509_get_notAfter(ucert));
  time_t time_diff = time_after - time_now ;

  if (time_diff < 0)
    Print(INFO) << std::endl << "Error: your certificate expired "
                << asctime(localtime(&time_after)) << std::endl << std::flush;
  else if (hours && time_diff < hours*60*60)
    Print(INFO) << "Warning: your certificate and proxy will expire "
                << asctime(localtime(&time_after))
                << "which is within the requested lifetime of the proxy"
                << std::endl << std::flush;
  
  time_t time_after_proxy;
    
  if (hours) 
    time_after_proxy = time_now + hours*60*60;
  else 
    time_after_proxy = time_after;
    
  Print(INFO) << "Your proxy is valid until "
              << asctime(localtime(&time_after_proxy)) << std::endl << std::flush;
}

bool Fake::Retrieve(VOLIST *volist) 
{
  AC **actmplist = NULL;
  AC *ac = NULL;
  int res = 0;
  BIO *hcrt = BIO_new(BIO_s_file()), 
      *hckey = BIO_new(BIO_s_file()),
      *owncert = BIO_new(BIO_s_file());
  X509 *hcert = NULL, *holder = NULL;
  EVP_PKEY *hkey = NULL;

  for (int i = 0; volist->vos[i]; i++) {
    VO *vo = volist->vos[i];

    // generic attributes
    char ** attributes = vo->gas;

    if (hcrt && hckey && owncert) {
      int hcertres = BIO_read_filename(hcrt, vo->hostcert);
      int holderres = BIO_read_filename(hckey, vo->hostkey);
      int hkeyres = BIO_read_filename(owncert, certfile);
      if ((hcertres  > 0) && (holderres > 0) && (hkeyres > 0)) {
        hcert = PEM_read_bio_X509(hcrt, NULL, 0, NULL);
        holder = PEM_read_bio_X509(owncert, NULL, 0, NULL);
        hkey = PEM_read_bio_PrivateKey(hckey, NULL, 0, NULL);
        
        if (hcert && hkey) {
          ac = AC_new();
          //          const char *uri = vo->uri ? vo->uri : "";

          // The following two lines allow the creation of an AC
          // without any FQAN.
          char *vector[1] = {NULL };
          char **fqanlist = vo->fqans ? vo->fqans : vector;
          int seconds = vo->pastac ? time_to_sec(vo->pastac) : 0;

          if (seconds == -1) {
            Print(ERROR) << "Minutes and seconds for VO: " << vo->voname <<
              " should be < 59 and >= 0" << std::endl;
            exit(1);
          }

          if (ac)
            res = writeac(hcert, NULL, holder, hkey, (BIGNUM *)(BN_value_one()), fqanlist,
                          vo->targets, attributes, &ac, vo->voname, vo->uri, vo->vomslife, !newformat, seconds);
        }
      } 
      else {
        if (hcertres <= 0) {
          if (vo->hostcert == NULL)
            Print(ERROR) << "Host credential file unspecified!" << std::endl;
          else
            Print(ERROR) << "Could not open host credential file: " << vo->hostcert << std::endl;
        }
        if (holderres <= 0) {
          if (vo->hostkey == NULL)
            Print(ERROR) << "Host key file unspecified!" << std::endl;
          else
            Print(ERROR) << "Could not open host key file: " << vo->hostkey << std::endl;
        }
        if (hkeyres <= 0) {
          if (certfile == NULL)
            Print(ERROR) << "Holder key file unspecified!" << std::endl;
          else
            Print(ERROR) << "Could not open holder key file: " << certfile << std::endl;
        }
        return false;
      }        
    }

    if (!res)
      actmplist = (AC **)listadd((char **)aclist, (char *)ac, sizeof(AC *));

    if (actmplist)
      aclist = actmplist;

    X509_free(hcert);
    X509_free(holder);
    EVP_PKEY_free(hkey);
    BIO_free(hcrt);
    BIO_free(hckey);
    BIO_free(owncert);

    hcrt = BIO_new(BIO_s_file());
    hckey = BIO_new(BIO_s_file());
    owncert = BIO_new(BIO_s_file());
  }

  if (!actmplist) {
    AC_free(ac);
    listfree((char **)aclist, (freefn)AC_free);

    Error();
    return false;
  }

  return true;
}

bool Fake::pcdInit() {

  int status = false;

  ERR_load_prxyerr_strings(0);
  SSLeay_add_ssl_algorithms();
  
  BIO * bio_err;
  if ((bio_err = BIO_new(BIO_s_file())) != NULL)
    BIO_set_fp(bio_err, stderr, BIO_NOCLOSE);


  if (!determine_filenames(&cacertfile, &certdir, &outfile, &certfile, &keyfile, noregen))
    goto err;
  
  Print(DEBUG) << "Files being used:" << std::endl 
               << " CA certificate file: " << (cacertfile ? cacertfile : "none") << std::endl
               << " Trusted certificates directory : " << (this->certdir ? this->certdir : "none") << std::endl
               << " Proxy certificate file : " << (this->outfile ? this->outfile : "none") << std::endl
               << " User certificate file: " << (this->certfile ? this->certfile : "none") << std::endl
               << " User key file: " << (this->keyfile ? this->keyfile : "none") << std::endl << std::flush;
  
  Print(DEBUG) << "Output to " << outfile << std::endl << std::flush;
  
  if (!load_credentials(certfile, keyfile, &ucert, &cert_chain, &upkey, pw_cb))
    goto err;
  
  status = true;
  
 err:

  Error();
  return status;
  
}

void Fake::Error() 
{
  std::string output = OpenSSLError(debug);

  if (debug)
    Print(DEBUG) << output;
  else
    Print(ERROR) << output;
}


void Fake::exitError(const char *string) 
{
  Print(ERROR) << string << std::endl;
  exit(1);
}

bool Fake::VerifyOptions()
{
  if (debug) {
    quiet = false;
    yydebug = 1;
  }

  if (!voms.empty()) {
    if (hostcert.empty())
      exitError("Error: You must specify an host certificate!");

    if (hostcert.empty() || hostkey.empty())
      exitError("Error: You must specify an host key!");
  }

  /* set globus version */

  version = globus(version);
  if (version == 0) {
    version = 22;
    Print(DEBUG) << "Unable to discover Globus version: trying for 2.2" << std::endl;
  }
  else 
    Print(DEBUG) << "Detected Globus version: " << version << std::endl;

  if (rfc && proxyver != 0) 
    exitError("Used both -rfc and --proxyver!\nChoose one or the other.");

  if (rfc)
    proxyver = 4;

  /* set proxy version */
  
  if (proxyver!=2 && proxyver!=3 && proxyver!=4 && proxyver!=0)
    exitError("Error: proxyver must be 2 or 3 or 4");
  else if (proxyver==0) {
    Print(DEBUG) << "Unspecified proxy version, settling on version: ";

    if (version<30)
      proxyver = 2;
    else if (version<40)
      proxyver = 3;
    else
      proxyver = 4;

    Print(DEBUG) << proxyver << std::endl;
  }

  /* PCI extension option */ 
  
  if (proxyver>3) {
    if (!policylang.empty())
      if (policyfile.empty())
        exitError("Error: if you specify a policy language you also need to specify a policy file");
  }
  
  if (proxyver>3) {
    Print(DEBUG) << "PCI extension info: " << std::endl << " Path length: " << pathlength << std::endl;

    if (policylang.empty())
      Print(DEBUG) << " Policy language not specified." << std::endl;
    else 
      Print(DEBUG) << " Policy language: " << policylang << std::endl;

    if (policyfile.empty())
      Print(DEBUG) << " Policy file not specified." << std::endl;
    else 
      Print(DEBUG) << " Policy file: " << policyfile << std::endl;
  }

  /* controls that number of bits for the key is appropiate */

  if ((bits!=512) && (bits!=1024) && 
      (bits!=2048) && (bits!=4096))
    exitError("Error: number of bits in key must be one of 512, 1024, 2048, 4096.");
  else 
    Print(DEBUG) << "Number of bits in key :" << bits << std::endl; 

  /* certificate duration option */
  
  if (hours < 0)
    exitError("Error: duration must be positive.");

  if (volist) {
    for (int i = 0; i < volist->current; i++) {
      VO *vo = volist->vos[i];
      if (!vo->voname)
        exitError("Error: You must give a name to a VO!");

      if (vo->hostcert == NULL)
        exitError("Error: You must specify an host certificate!");

      if (vo->hostkey == NULL)
        exitError("Error: You must specify an host key!");

      if (vo->vomslife < 0)
        exitError("Error: Duration of AC must be positive.");
    }
  }

  return true;
}

struct nullstream: std::ostream {
  struct nullbuf: std::streambuf {
    int overflow(int c) { return traits_type::not_eof(c); }
  } m_sbuf;
  nullstream(): std::ios(&m_sbuf), std::ostream(&m_sbuf) {}
};

nullstream voidstream;

std::ostream& Fake::Print(message_type type) 
{
  if (type == FORCED)
    return std::cout;

  if (type == ERROR)
    return std::cerr;

  if (quiet)
    return voidstream;

  if (type == WARN)
    return std::cerr;

  if (type == DEBUG && !debug)
    return voidstream;

  return std::cout;
}

static int time_to_sec(std::string timestring)
{
  int seconds = 0;
  int hours   = 0;
  int minutes = 0;

  std::string::size_type pos = timestring.find(':');

  if (pos == std::string::npos) {
    /* Seconds format */
    seconds = mystrtol((char*)timestring.c_str(), LONG_MAX);
  } 
  else {
    /* hours:minutes(:seconds) format */
    hours   = mystrtol((char*)timestring.substr(0, pos).c_str(), LONG_MAX);

    std::string::size_type pos2 = timestring.substr(pos+1).find(':');

    if (pos2 == std::string::npos) {
      minutes = mystrtol((char*)timestring.substr(pos+1).c_str(), 59);
    }
    else {
      minutes = mystrtol((char*)timestring.substr(pos+1, pos2).c_str(), 59);
      seconds = mystrtol((char*)timestring.substr(pos2+1).c_str(), 59);
    }
  }

  if (seconds == -1 || minutes == -1 || hours == -1)
    return -1;

  return seconds + minutes * 60 + hours * 3600;
}

static long mystrtol(char *number, int limit)
{
  char *end = NULL;

  errno = 0;

  long value = strtol(number, &end, 10);

  /* Was there extraneous data at the end ? */
  if (end - number != strlen(number))
    return -1;

  /* Conversion errors of some kind */
  if (errno != 0 || value < 0)
    return -1;

  /* Value greater than maximum */
  if (value > limit)
    return -1;

  return value;
}
