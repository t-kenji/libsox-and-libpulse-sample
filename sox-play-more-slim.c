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

typedef enum {RG_off, RG_track, RG_album, RG_default} rg_mode;
static lsx_enum_item const rg_modes[] = {
  LSX_ENUM_ITEM(RG_,off)
  LSX_ENUM_ITEM(RG_,track)
  LSX_ENUM_ITEM(RG_,album)
  {0, 0}};

/* Input & output files */

typedef struct {
  char * filename;

  /* fopts */
  char const * filetype;
  sox_signalinfo_t signal;
  sox_encodinginfo_t encoding;
  double volume;
  double replay_gain;
  sox_oob_t oob;
  sox_bool no_glob;

  sox_format_t * ft;  /* libSoX file descriptor */
  uint64_t volume_clips;
} file_t;

static file_t ifile_, *ifile = &ifile_;
static file_t ofile_, *ofile = &ofile_;

/* Effects */

/* We parse effects into a temporary effects table and then place into
 * the real effects chain.  This allows scanning all effects to give
 * hints to what input effect options should be as well as determining
 * when mixer or resample effects need to be auto-inserted as well.
 */
static sox_effects_chain_t *effects_chain = NULL;

/* Flowing */

static sox_signalinfo_t combiner_signal, ofile_signal_options;
static sox_encodinginfo_t combiner_encoding, ofile_encoding_options;
static uint64_t input_wide_samples = 0;
static uint64_t read_wide_samples = 0;
static uint64_t output_samples = 0;
static sox_bool input_eof = sox_false;
static sox_bool output_eof = sox_false;
static sox_bool user_abort = sox_false;
static sox_bool user_skip = sox_false;
static sox_bool user_restart_eff = sox_false;
static int success = 0;
static int cleanup_called = 0;

static sox_bool is_guarded;

struct timeval load_timeofday;

#define DEBUG(fmt,...) do{fprintf(stderr,"%s:%d " fmt "\n",__FILE__,__LINE__,##__VA_ARGS__);}while(0)

static void cleanup(void)
{
DEBUG("%s called", __func__);
  /* Close the input and output files before exiting. */
  if (ifile->ft) {
    sox_close(ifile->ft);
  }
  free(ifile->filename);

  if (ofile->ft) {
    sox_close(ofile->ft);
  }
  free(ofile->filename);

  free(sox_globals.tmp_path);
  sox_globals.tmp_path = NULL;

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

static void progress_to_next_input_file(file_t * f, sox_effect_t * effp)
{
  if (user_skip) {
    user_skip = sox_false;
    fprintf(stderr, "\nSkipped (Ctrl-C twice to quit).\n");
  }
DEBUG("%s called", __func__);
  read_wide_samples = 0;
  input_wide_samples = f->ft->signal.length / f->ft->signal.channels;
  if (f->volume == HUGE_VAL) {
    f->volume = 1;
DEBUG("volume: %f", f->volume);
  }
  if (f->replay_gain != HUGE_VAL) {
    f->volume *= pow(10.0, f->replay_gain / 20);
DEBUG("volume: %f", f->volume);
  }
  if (effp && f->volume != floor(f->volume)) {
    effp->out_signal.precision = SOX_SAMPLE_PRECISION;
DEBUG("precision: %d", effp->out_signal.precision);
  }
  f->ft->sox_errno = errno = 0;
}

/* Read up to max `wide' samples.  A wide sample contains one sample per channel
 * from the input audio. */
static size_t sox_read_wide(sox_format_t * ft, sox_sample_t * buf, size_t max)
{
  size_t len = max / combiner_signal.channels;
DEBUG("%s called", __func__);
  len = sox_read(ft, buf, len * ft->signal.channels) / ft->signal.channels;
  if (!len && ft->sox_errno)
    lsx_fail("`%s' %s: %s",
        ft->filename, ft->sox_errstr, sox_strerror(ft->sox_errno));
  return len;
}

static void balance_input(sox_sample_t * buf, size_t ws, file_t * f)
{
  size_t s = ws * f->ft->signal.channels;

DEBUG("%s called", __func__);
  if (f->volume != 1) while (s--) {
    double d = f->volume * *buf;
DEBUG("%s d: %f", __func__, d);
    *buf++ = SOX_ROUND_CLIP_COUNT(d, f->volume_clips);
  }
}

/* The input combiner: contains one sample buffer per input file, but only
 * needed if is_parallel(combine_method) */
typedef struct {
  sox_sample_t * * ibuf;
  size_t *         ilen;
} input_combiner_t;

static int combiner_start(sox_effect_t *effp)
{
  input_combiner_t * z = (input_combiner_t *) effp->priv;

DEBUG("%s called", __func__);
  progress_to_next_input_file(ifile, effp);
  z->ilen = lsx_malloc(sizeof(*z->ilen));
  return SOX_SUCCESS;
}

static int combiner_drain(sox_effect_t *effp, sox_sample_t * obuf, size_t * osamp)
{
  size_t olen = 0;

DEBUG("%s called", __func__);
  while (sox_true) {
    if (!user_skip)
      olen = sox_read_wide(ifile->ft, obuf, *osamp);
    balance_input(obuf, olen, ifile);
    break;
  } /* while */
  read_wide_samples += olen;
  olen *= effp->in_signal.channels;
  *osamp = olen;

  input_eof = olen ? sox_false : sox_true;

  return olen? SOX_SUCCESS : SOX_EOF;
}

static int combiner_stop(sox_effect_t *effp)
{
  input_combiner_t * z = (input_combiner_t *) effp->priv;

DEBUG("%s called", __func__);
  free(z->ilen);

  return SOX_SUCCESS;
}

static sox_effect_handler_t const * input_combiner_effect_fn(void)
{
  static sox_effect_handler_t handler = { "input", 0, SOX_EFF_MCHAN |
    SOX_EFF_MODIFY, 0, combiner_start, 0, combiner_drain,
    combiner_stop, 0, sizeof(input_combiner_t)
  };
DEBUG("%s called", __func__);
  return &handler;
}

static int ostart(sox_effect_t *effp)
{
  unsigned prec = effp->out_signal.precision;
  if (effp->in_signal.mult && effp->in_signal.precision > prec) {
    *effp->in_signal.mult *= 1 - (1 << (31 - prec)) * (1. / SOX_SAMPLE_MAX);
DEBUG("%s mult: %f", __func__, *effp->in_signal.mult);
  }
DEBUG("%s called", __func__);
  return SOX_SUCCESS;
}

static int output_flow(sox_effect_t *effp, sox_sample_t const * ibuf,
    sox_sample_t * obuf, size_t * isamp, size_t * osamp)
{
  size_t len;

DEBUG("%s called", __func__);
  (void)effp, (void)obuf;
  *osamp = 0;
  len = *isamp? sox_write(ofile->ft, ibuf, *isamp) : 0;
  output_samples += len / ofile->ft->signal.channels;
  output_eof = (len != *isamp) ? sox_true: sox_false;
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

static void auto_effect(sox_effects_chain_t *, char const *, int, char **,
    sox_signalinfo_t *, int *);

static int add_effect(sox_effects_chain_t * chain, sox_effect_t * effp,
    sox_signalinfo_t * in, sox_signalinfo_t const * out, int * guard) {
  int no_guard = -1;
DEBUG("%s called", __func__);
  switch (*guard) {
    case 0: if (!(effp->handler.flags & SOX_EFF_GAIN)) {
      char * arg = "-h";
DEBUG("%s called", __func__);
      auto_effect(chain, "gain", 1, &arg, in, &no_guard);
      ++*guard;
    }
    break;
    case 1: if (effp->handler.flags & SOX_EFF_GAIN) {
      char * arg = "-r";
DEBUG("%s called", __func__);
      auto_effect(chain, "gain", 1, &arg, in, &no_guard);
      --*guard;
    }
    break;
    case 2: if (!(effp->handler.flags & SOX_EFF_MODIFY)) {
      lsx_warn("%s: effects that modify audio should not follow dither",
        effp->handler.name);
    }
    break;
  }
  return sox_add_effect(chain, effp, in, out);
}

static void auto_effect(sox_effects_chain_t *chain, char const *name, int argc,
    char *argv[], sox_signalinfo_t *signal, int * guard)
{
  sox_effect_t * effp;

DEBUG("%s effect: %s", __func__, name);
  effp = sox_create_effect(sox_find_effect(name)); /* Should always succeed. */

  if (sox_effect_options(effp, argc, argv) == SOX_EOF)
    exit(1); /* The failing effect should have displayed an error message */

  if (add_effect(chain, effp, signal, &ofile->ft->signal, guard) != SOX_SUCCESS)
    exit(2); /* The effects chain should have displayed an error message */
  free(effp);
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
  sox_signalinfo_t signal = combiner_signal;
  int guard = is_guarded - 1;
  sox_effect_t * effp;
  char * rate_arg = "-l";

DEBUG("%s called", __func__);
  /* 1st `effect' in the chain is the input combiner_signal.
   * add it only if its not there from a previous run.  */
  if (chain->length == 0) {
DEBUG("%s called", __func__);
    effp = sox_create_effect(input_combiner_effect_fn());
    sox_add_effect(chain, effp, &signal, &ofile->ft->signal);
    free(effp);
  }

  /* Add auto effects if still needed at this point */
  if (signal.channels < ofile->ft->signal.channels &&
      signal.rate != ofile->ft->signal.rate) {
DEBUG("%s called", __func__);
    auto_effect(chain, "rate", 1, &rate_arg, &signal, &guard);
  }
  if (signal.channels != ofile->ft->signal.channels) {
DEBUG("%s called", __func__);
    auto_effect(chain, "channels", 0, NULL, &signal, &guard);
  }
  if (signal.rate != ofile->ft->signal.rate) {
DEBUG("%s called", __func__);
    auto_effect(chain, "rate", 1, &rate_arg, &signal, &guard);
  }

  /* Last `effect' in the chain is the output file */
  effp = sox_create_effect(output_effect_fn());
  if (sox_add_effect(chain, effp, &signal, &ofile->ft->signal) != SOX_SUCCESS)
    exit(2);
  free(effp);
}

static int update_status(sox_bool all_done, void * client_data)
{
  return (user_abort || user_restart_eff) ? SOX_EOF : SOX_SUCCESS;
}

static void open_output_file(void)
{
  double factor;
  int i;
  sox_oob_t oob = ifile->ft->oob;
  char *expand_fn;

DEBUG("%s called", __func__);

  /* Copy loop info, resizing appropriately it's in samples, so # channels
   * don't matter FIXME: This doesn't work for multi-file processing or effects
   * that change file length.  */
  factor = (double) ofile->signal.rate / combiner_signal.rate;
  for (i = 0; i < SOX_MAX_NLOOPS; i++) {
    oob.loops[i].start = oob.loops[i].start * factor;
    oob.loops[i].length = oob.loops[i].length * factor;
DEBUG("i: %d, start: %lu, length: %lu", i, oob.loops[i].start, oob.loops[i].length);
  }

  expand_fn = lsx_strdup(ofile->filename);
DEBUG("filename: %s, filetype: %s", expand_fn, ofile->filetype);
  ofile->ft = sox_open_write(expand_fn, &ofile->signal, &ofile->encoding,
      ofile->filetype, &oob, NULL);
  sox_delete_comments(&oob.comments);
  free(expand_fn);

  if (!ofile->ft)
    /* sox_open_write() will call lsx_warn for most errors.
     * Rely on that printing something. */
    exit(2);
}

static void sigint(int s)
{
  user_abort = sox_true;
}

static void calculate_combiner_signal_parameters(void)
{
  /* Set the combiner output signal attributes to those of the 1st/next input
   * file.  If we are in sox_sequence mode then we don't need to check the
   * attributes of the other inputs, otherwise, it is mandatory that all input
   * files have the same sample rate, and for sox_concatenate, it is mandatory
   * that they have the same number of channels, otherwise, the number of
   * channels at the output of the combiner is calculated according to the
   * combiner mode. */
DEBUG("%s called", __func__);
  combiner_signal = ifile->ft->signal;
  size_t max_channels = 0;
  uint64_t total_length = 0;

  /* Report all input files and gather info on differing rates & numbers of
   * channels, and on the resulting output audio length: */
  max_channels = ifile->ft->signal.channels;
DEBUG("length: %lu", ifile->ft->signal.length);
  total_length = ifile->ft->signal.length;

  /* Store the calculated # of combined channels: */
  combiner_signal.channels = max_channels;
DEBUG("combiner_signal.channels: %u", combiner_signal.channels);

  combiner_signal.length = total_length;
DEBUG("combiner_signal.length: %lu", combiner_signal.length);
} /* calculate_combiner_signal_parameters */

static void calculate_output_signal_parameters(void)
{
  sox_bool known_length = sox_true;
  uint64_t olen = 0;

  /* Report all input files and gather info on differing rates & numbers of
   * channels, and on the resulting output audio length: */
DEBUG("%s called", __func__);
  olen = ifile->ft->signal.length / ifile->ft->signal.channels;

  /* Determine the output file signal attributes; set from user options
   * if given: */
  ofile->signal = ofile_signal_options;

  /* If no user option for output rate or # of channels, set from the last
   * effect that sets these, or from the input combiner if there is none such */
  if (!ofile->signal.rate)
    ofile->signal.rate = combiner_signal.rate;
  if (!ofile->signal.channels)
    ofile->signal.channels = combiner_signal.channels;

  /* FIXME: comment this: */
  ofile->signal.precision = combiner_signal.precision;

  if (!known_length)
    olen = 0;
  ofile->signal.length = (uint64_t)(olen * ofile->signal.channels * ofile->signal.rate / combiner_signal.rate + .5);
}

static void set_combiner_and_output_encoding_parameters(void)
{
DEBUG("%s called", __func__);
  /* The input encoding parameters passed to the effects chain are those of
   * the first input file (for each segued block if sox_sequence):*/
  combiner_encoding = ifile->ft->encoding;

  /* Determine the output file encoding attributes; set from user options
   * if given: */
  ofile->encoding = ofile_encoding_options;

  /* Get unspecified output file encoding attributes from the input file and
   * set the output file to the resultant encoding if this is supported by the
   * output file type; if not, the output file handler should select an
   * encoding suitable for the output signal and its precision. */
  {
    sox_encodinginfo_t t = ofile->encoding;
    if (!t.encoding)
      t.encoding = combiner_encoding.encoding;
    if (!t.bits_per_sample)
      t.bits_per_sample = combiner_encoding.bits_per_sample;
    if (sox_format_supports_encoding(ofile->filename, ofile->filetype, &t))
      ofile->encoding = t;
  }
}

static int process(void)
{         /* Input(s) -> Balancing -> Combiner -> Effects -> Output */
  int flow_status;

DEBUG("%s called", __func__);
  calculate_combiner_signal_parameters();
  set_combiner_and_output_encoding_parameters();
  calculate_output_signal_parameters();
  open_output_file();

  if (!effects_chain) {
DEBUG("%s trace", __func__);
    effects_chain = sox_create_effects_chain(&combiner_encoding,
                                             &ofile->ft->encoding);
  }
  add_effects(effects_chain);

  signal(SIGTERM, sigint); /* Stop gracefully, as soon as we possibly can. */
  signal(SIGINT , sigint); /* Either skip current input or behave as SIGTERM. */
  flow_status = sox_flow_effects(effects_chain, update_status, NULL);

  return flow_status;
}

enum {ENDIAN_little, ENDIAN_big, ENDIAN_swap};
static lsx_enum_item const endian_options[] = {
  LSX_ENUM_ITEM(ENDIAN_,little)
  LSX_ENUM_ITEM(ENDIAN_,big)
  LSX_ENUM_ITEM(ENDIAN_,swap)
  {0, 0}};

static lsx_enum_item const plot_methods[] = {
  LSX_ENUM_ITEM(sox_plot_,off)
  LSX_ENUM_ITEM(sox_plot_,octave)
  LSX_ENUM_ITEM(sox_plot_,gnuplot)
  LSX_ENUM_ITEM(sox_plot_,data)
  {0, 0}};

enum {
  encoding_signed_integer, encoding_unsigned_integer, encoding_floating_point,
  encoding_ms_adpcm, encoding_ima_adpcm, encoding_oki_adpcm,
  encoding_gsm_full_rate, encoding_u_law, encoding_a_law};

static void init_file(file_t * f)
{
DEBUG("%s called", __func__);
  memset(f, 0, sizeof(*f));
  sox_init_encodinginfo(&f->encoding);
  f->volume = HUGE_VAL;
  f->replay_gain = HUGE_VAL;
}

int main(int argc, char **argv)
{
  gettimeofday(&load_timeofday, NULL);

  if (sox_init() != SOX_SUCCESS)
    exit(1);

  errno = 0; /* Both isatty & fileno may set errno. */

  atexit(atexit_cleanup);

{
DEBUG("%s called", __func__);
  file_t opts;
  init_file(&opts);
  *ifile = opts;
  ifile->filename = lsx_strdup(argv[1]);
  init_file(&opts);
  opts.filetype = "pulseaudio";
  *ofile = opts;
  ofile->filename = lsx_strdup("default");
}
  signal(SIGINT, SIG_IGN); /* So child pipes aren't killed by track skip */
  ifile->ft = sox_open_read(ifile->filename, &ifile->signal, &ifile->encoding, ifile->filetype);
  if (!ifile->ft) {
    exit(2);
  }

  signal(SIGINT, SIG_DFL);

  if (!sox_globals.repeatable) {/* Re-seed PRNG? */
    struct timeval now;
    gettimeofday(&now, NULL);
    sox_globals.ranqd1 = (int32_t)(now.tv_sec - now.tv_usec);
  }

  /* Save things that sox_sequence needs to be reinitialised for each segued
   * block of input files.*/
  ofile_signal_options = ofile->signal;
  ofile_encoding_options = ofile->encoding;

  process();

  sox_delete_effects_chain(effects_chain);

  success = 1; /* Signal success to cleanup so the output file isn't removed. */

  cleanup();

  return 0;
}
