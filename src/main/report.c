/*
 * Copyright (c) 2012, LiteStack, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <sys/time.h>
#include <sys/mman.h>
#include "src/main/report.h"
#include "src/platform/signal.h"
#include "src/main/accounting.h"
#include "src/main/setup.h"
#include "src/channels/channel.h"

#define QUANT MICRO_PER_SEC

#ifdef DEBUG
#define REPORT_VALIDATOR "validator state = "
#define REPORT_DAEMON "daemon = "
#define REPORT_RETCODE "user return code = "
#define REPORT_ETAG "etag(s) = "
#define REPORT_ACCOUNTING "accounting = "
#define REPORT_STATE "exit state = "
#define REPORT_CMD cmd->str
#define EOL "\r"
#else
#define REPORT_VALIDATOR ""
#define REPORT_DAEMON ""
#define REPORT_RETCODE ""
#define REPORT_ETAG ""
#define REPORT_ACCOUNTING ""
#define REPORT_STATE ""
#define REPORT_CMD ""
#define EOL "\n"
#endif

/*
 * 0: to /dev/stdout, 1: to syslog, 2: (0) + fast reports, 3: to socket
 * TODO(d'b): fast reports should be cut from report output
 * TODO(d'b): report outputs should be enumerated (magic numbers to remove)
 */
static int report_mode = 0;
static int zvm_code = 0;
static int user_code = 0;
static int validation_state = 2;
static int daemon_state = 0;
static char *zvm_state = NULL;
static GString *digests = NULL; /* cumulative etags */
static GString *cmd = NULL;
static int report_handle = STDOUT_FILENO;

void SetReportHandle(int handle)
{
  report_handle = handle;
}

void ReportMode(int mode)
{
  report_mode = mode;
}

void SetExitState(const char *state)
{
  g_free(zvm_state);
  zvm_state = g_strdup(state);
}

void SetExitCode(int code)
{
  /* only the 1st error matters */
  if(zvm_code == 0) zvm_code = code;
}

int GetExitCode()
{
  return zvm_code;
}

void SetUserCode(int code)
{
  user_code = code;
}

void SetValidationState(int state)
{
  validation_state = state;
}

void SetDaemonState(int state)
{
  daemon_state = state;
}

void SetCmdString(GString *s)
{
  cmd = s;
}

/* log user memory map */
static void LogMemMap(struct NaClApp *nap, int verbosity)
{
  int i;

  ZLOGS(verbosity, "user memory map (in pages):");

  for(i = 0; i < MemMapSize; ++i)
    ZLOGS(verbosity, "%s: address = 0x%08x, size = 0x%08x, protection = x",
        nap->mem_map[i].name, (uint32_t) nap->mem_map[i].start,
        (uint32_t) nap->mem_map[i].size, nap->mem_map[i].prot);
}

static void FinalDump(struct NaClApp *nap)
{
  ZLOGS(LOG_INSANE, "exiting -- printing NaClApp details");
  PrintAppDetails(nap, LOG_INSANE);
  LogMemMap(nap, LOG_INSANE);

  SignalHandlerFini();
}

void ReportTag(char *name, void *tag)
{
  char digest[TAG_DIGEST_SIZE + 1];

  assert(name != NULL);
  if(tag == NULL) return;

  TagDigest(tag, digest);
  g_string_append_printf(digests, "%s %s ", name, digest);
}

/* calculate user memory tag, and return pointer to it */
static void *GetMemoryDigest(struct NaClApp *nap)
{
  int i;

  assert(nap != NULL);

  /* calculate overall memory tag */
  for(i = 0; i < MemMapSize; ++i)
  {
    uintptr_t addr = nap->mem_map[i].start;
    int64_t size = nap->mem_map[i].size;

    /* update user_etag skipping inaccessible pages */
    if(nap->mem_map[i].prot & PROT_READ)
      TagUpdate(nap->manifest->mem_tag, (const char*)addr, size);
  }

  return nap->manifest->mem_tag;
}

void ReportCtor()
{
  digests = g_string_sized_new(BIG_ENOUGH_STRING);
}

/* output report */
/* TODO(d'b): rework "-t" and update the function */
static void OutputReport(char *r)
{
  char *p = NULL;
  int size = strlen(r);

#define REPORT(p) ZLOGIF(write(report_handle, p, size) != size, \
  "report write error %d: %s", errno, strerror(errno))

  switch(report_mode)
  {
    case 3: /* unix socket */
      p = g_strdup_printf("0x%06x%s", size, r);
      size = strlen(p);
      REPORT(p);
      g_free(p);
      break;
    case 0: /* stdout */
      REPORT(r);
      break;
    case 1: /* syslog */
      ZLOGS(LOG_ERROR, "%s", r);
      break;
    default:
      ZLOG(LOG_ERROR, "invalid report mode %d", report_mode);
      break;
  }
#undef REPORT
}

void FastReport()
{
  char *eol = report_mode == 1 ? "; " : EOL;
  static int64_t start = 0;
  int64_t now = 0;
  struct timeval t;
  char *acc = NULL;
  char *r = NULL;

  /* skip fast report if specified */
  if(report_mode != 2) return;

  /* check if it is time to report */
  gettimeofday(&t, NULL);
  now = MICRO_PER_SEC * t.tv_sec + t.tv_usec;
  if(now - start < QUANT) return;
  start += start == 0 ? now : QUANT;

  /* create and output report */
  acc = FastAccounting();
  r = g_strdup_printf("%s%s%s", REPORT_ACCOUNTING, acc, eol);
  OutputReport(r);

  g_free(acc);
  g_free(r);
}

/* part of report class dtor */
#define REPORT g_string_append_printf
void Report(struct NaClApp *nap)
{
  GString *r = g_string_sized_new(BIG_ENOUGH_STRING);
  char *eol = report_mode == 1 ? "; " : "\n";
  char *acc = FinalAccounting();

  /* report validator state and user return code */
  REPORT(r, "%s%d%s", REPORT_VALIDATOR, validation_state, eol);
  REPORT(r, "%s%d%s", REPORT_DAEMON, daemon_state, eol);
  REPORT(r, "%s%d%s", REPORT_RETCODE, user_code, eol);

  /* add memory digest to cumulative digests if asked */
  if(nap != NULL && nap->manifest != NULL)
    if(nap->manifest->mem_tag != NULL)
      ReportTag(STDRAM, GetMemoryDigest(nap));

  /* report tags digests and remove ending " " if exist */
  REPORT(r, "%s", REPORT_ETAG);
  REPORT(r, "%s", digests->len == 0
      ? TAG_ENGINE_DISABLED : digests->str);
  g_string_truncate(r, r->len - 1);

  /* report accounting and session message */
  if(zvm_state == NULL) zvm_state = UNKNOWN_STATE;
  REPORT(r, "%s%s%s%s", eol, REPORT_ACCOUNTING, acc, eol);
  REPORT(r, "%s%s%s", REPORT_STATE,
      zvm_state == NULL ? UNKNOWN_STATE : zvm_state, eol);
  REPORT(r, "%s%s", REPORT_CMD, eol);
  OutputReport(r->str);

  g_string_free(r, TRUE);
  g_free(acc);
}

void ReportDtor(int zvm_ret)
{
  SetExitCode(zvm_ret);

  /* broken session */
  if(zvm_code != 0)
  {
    ZLOGS(LOG_ERROR, "SESSION %d FAILED WITH ERROR %d: %s",
        gnap->manifest == NULL ? 0 : gnap->manifest->node,
        zvm_code, strerror(zvm_code));
    FinalDump(gnap);
    ZTrace("[final dump]");
  }

  ChannelsDtor(gnap->manifest);
  ZTrace("[channels destruction]");
  Report(gnap);
  ZTrace("[report]");
  NaClAppDtor(gnap); /* free user space and globals */
  ZTrace("[untrusted context closing]");
  ManifestDtor(gnap->manifest); /* dispose manifest and channels */
  ZTrace("[manifest deallocating]");
  FreeDispatchThunk();
  ZTrace("[thunk deallocating]");
  ZLogDtor();
  ZTrace("[zlog deallocating]");

  /* free local resources and exit */
  g_string_free(digests, TRUE);
  g_string_free(cmd, TRUE);
  g_free(zvm_state);

  ZTrace("[exit]");
  ZTraceDtor(1);
  ZTraceNameDtor();
  _exit(zvm_code);
}
