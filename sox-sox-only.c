#include "sox.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <glob.h>
#include <sys/soundcard.h>
#include <sys/time.h>
#include <sys/timeb.h>
#include <sys/utsname.h>
#include <unistd.h>

typedef struct {
  char * filename;

  /* fopts */
  char const * filetype;
  sox_signalinfo_t signal;
  sox_encodinginfo_t encoding;
  sox_oob_t oob;

  sox_format_t * ft;  /* libSoX file descriptor */
} file_t;

static file_t ifile_, *ifile = &ifile_;
static file_t ofile_, *ofile = &ofile_;

static sox_effects_chain_t *effects_chain = NULL;

static sox_bool user_abort = sox_false;
static int success = 0;
static int cleanup_called = 0;

#define DEBUG(fmt,...) do{fprintf(stderr,"%s:%d " fmt "\n",__FILE__,__LINE__,##__VA_ARGS__);}while(0)

static void cleanup(void)
{
DEBUG("%s called", __func__);
  /* Close the input and output files before exiting. */
  if (ifile->ft) {
    sox_close(ifile->ft);
  }
  if (ofile->ft) {
    sox_close(ofile->ft);
  }
  sox_quit();

  cleanup_called = 1;
}

/* Cleanup atexit() function, hence always called. */
static void atexit_cleanup(void)
{
DEBUG("%s called", __func__);
  /* Do not call cleanup using atexit() if possible.  pthread's can
   * act unpredictable if called outside of main().
   */
  if (!cleanup_called)
    cleanup();
}

/* Read up to max `wide' samples.  A wide sample contains one sample per channel
 * from the input audio. */
static size_t sox_read_wide(sox_format_t * ft, sox_sample_t * buf, size_t max)
{
  size_t len = max / ifile->ft->signal.channels;
  len = sox_read(ft, buf, len * ft->signal.channels) / ft->signal.channels;
DEBUG("%s: max: %lu, len=%lu", __func__, max, len);
  if (!len && ft->sox_errno)
    lsx_fail("`%s' %s: %s",
        ft->filename, ft->sox_errstr, sox_strerror(ft->sox_errno));
  return len;
}

static int combiner_start(sox_effect_t *effp)
{
DEBUG("%s called", __func__);
  return SOX_SUCCESS;
}

static int combiner_drain(sox_effect_t *effp, sox_sample_t * obuf, size_t * osamp)
{
  size_t olen = 0;

DEBUG("%s osamp: %lu", __func__, *osamp);
  olen = sox_read_wide(ifile->ft, obuf, *osamp);
  olen *= effp->in_signal.channels;
  *osamp = olen;

  return olen? SOX_SUCCESS : SOX_EOF;
}

static int combiner_stop(sox_effect_t *effp)
{
DEBUG("%s called", __func__);
  return SOX_SUCCESS;
}

static sox_effect_handler_t const * input_combiner_effect_fn(void)
{
  static sox_effect_handler_t handler = { "input", 0, SOX_EFF_MCHAN |
    SOX_EFF_MODIFY, 0, combiner_start, 0, combiner_drain,
    combiner_stop, 0, 0
  };
DEBUG("%s called", __func__);
  return &handler;
}

static int ostart(sox_effect_t *effp)
{
DEBUG("%s called", __func__);
  return SOX_SUCCESS;
}

static int output_flow(sox_effect_t *effp, sox_sample_t const * ibuf,
    sox_sample_t * obuf, size_t * isamp, size_t * osamp)
{
  size_t len;

  (void)effp, (void)obuf;
  *osamp = 0;
  len = *isamp? sox_write(ofile->ft, ibuf, *isamp) : 0;
DEBUG("%s isamp: %lu, len: %lu", __func__, *isamp, len);
  if (len != *isamp) {
    if (ofile->ft->sox_errno)
      lsx_fail("`%s' %s: %s", ofile->ft->filename,
          ofile->ft->sox_errstr, sox_strerror(ofile->ft->sox_errno));
    return SOX_EOF;
  }
  return SOX_SUCCESS;
}

static sox_effect_handler_t const * output_effect_fn(void)
{
  static sox_effect_handler_t handler = {"output", 0, SOX_EFF_MCHAN |
    SOX_EFF_MODIFY | SOX_EFF_PREC, NULL, ostart, output_flow, NULL, NULL, NULL, 0
  };
DEBUG("%s called", __func__);
  return &handler;
}

/* Add all user effects to the chain.  If the output effect's rate or
 * channel count do not match the end of the effects chain then
 * insert effects to correct this.
 *
 * This can be called with the input effect already in the effects
 * chain from a previous run.  Also, it use a pre-existing
 * output effect if its been saved into save_output_eff.
 */
static void add_effects(sox_effects_chain_t *chain)
{
  sox_signalinfo_t signal = ifile->ft->signal;
  sox_effect_t * effp;

DEBUG("%s trace", __func__);
  /* 1st `effect' in the chain is the input combiner_signal.
   * add it only if its not there from a previous run.  */
  if (chain->length == 0) {
DEBUG("%s called", __func__);
    effp = sox_create_effect(input_combiner_effect_fn());
    sox_add_effect(chain, effp, &signal, &ofile->ft->signal);
    free(effp);
  }

  /* Last `effect' in the chain is the output file */
  effp = sox_create_effect(output_effect_fn());
  if (sox_add_effect(chain, effp, &signal, &ofile->ft->signal) != SOX_SUCCESS)
    exit(2);
  free(effp);
}

static int update_status(sox_bool all_done, void * client_data)
{
  return user_abort ? SOX_EOF : SOX_SUCCESS;
}

static void sigint(int s)
{
  user_abort = sox_true;
}

static void calculate_output_signal_parameters(void)
{
  sox_bool known_length = sox_true;
  uint64_t olen = 0;

  /* Report all input files and gather info on differing rates & numbers of
   * channels, and on the resulting output audio length: */
DEBUG("%s called", __func__);
  olen = ifile->ft->signal.length / ifile->ft->signal.channels;

  /* If no user option for output rate or # of channels, set from the last
   * effect that sets these, or from the input combiner if there is none such */
  if (!ofile->signal.rate)
    ofile->signal.rate = ifile->ft->signal.rate;
  if (!ofile->signal.channels)
    ofile->signal.channels = ifile->ft->signal.channels;

  /* FIXME: comment this: */
  ofile->signal.precision = ifile->ft->signal.precision;

  if (!known_length)
    olen = 0;
  ofile->signal.length = (uint64_t)(olen * ofile->signal.channels * ofile->signal.rate / ifile->ft->signal.rate + .5);
DEBUG("%s olen: %lu, length: %lu", __func__, olen, ofile->signal.length);
}

static void set_combiner_and_output_encoding_parameters(void)
{
DEBUG("%s called", __func__);
  sox_encodinginfo_t t = ofile->encoding;
  if (!t.encoding)
    t.encoding = ifile->ft->encoding.encoding;
  if (!t.bits_per_sample)
    t.bits_per_sample = ifile->ft->encoding.bits_per_sample;
  if (sox_format_supports_encoding(ofile->filename, ofile->filetype, &t))
    ofile->encoding = t;
}

int main(int argc, char **argv)
{
  if (sox_init() != SOX_SUCCESS)
    exit(1);

  atexit(atexit_cleanup);

  memset(ifile, 0, sizeof(*ifile));
  sox_init_encodinginfo(&ifile->encoding);
  ifile->filename = argv[1];

  memset(ofile, 0, sizeof(*ofile));
  sox_init_encodinginfo(&ofile->encoding);
  ofile->filename = argv[2];

  signal(SIGINT, SIG_IGN); /* So child pipes aren't killed by track skip */
  ifile->ft = sox_open_read(ifile->filename, &ifile->signal, &ifile->encoding, ifile->filetype);
  if (!ifile->ft) {
    exit(2);
  }

  signal(SIGINT, SIG_DFL);

  set_combiner_and_output_encoding_parameters();
  calculate_output_signal_parameters();
  
  sox_oob_t oob = ifile->ft->oob;
DEBUG("filename: %s, filetype: %s", ofile->filename, ofile->filetype);
  ofile->ft = sox_open_write(ofile->filename, &ofile->signal, &ofile->encoding,
      ofile->filetype, &oob, NULL);

  if (!ofile->ft)
    /* sox_open_write() will call lsx_warn for most errors.
     * Rely on that printing something. */
    exit(2);

  if (!effects_chain) {
DEBUG("%s trace", __func__);
    effects_chain = sox_create_effects_chain(&ifile->ft->encoding,
                                             &ofile->ft->encoding);
  }
  add_effects(effects_chain);

  signal(SIGTERM, sigint); /* Stop gracefully, as soon as we possibly can. */
  signal(SIGINT , sigint); /* Either skip current input or behave as SIGTERM. */
  sox_flow_effects(effects_chain, update_status, NULL);

  sox_delete_effects_chain(effects_chain);

  success = 1; /* Signal success to cleanup so the output file isn't removed. */

  cleanup();

  return 0;
}
