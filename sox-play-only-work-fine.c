/* SoX - The Swiss Army Knife of Audio Manipulation.
 *
 * This is the main function for the SoX command line programs:
 *   sox, play.
 *
 * Copyright 1998-2009 Chris Bagwell and SoX contributors
 * Copyright 1991 Lance Norskog And Sundry Contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

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

#define TIME_FRAC 1e6

#define SOX_OPTS "SOX_OPTS"
static lsx_getopt_t optstate;

/* argv[0] options */

static char const * myname = NULL;
static enum {sox_sox, sox_play} sox_mode;


/* gopts */

static enum {
  sox_sequence, sox_concatenate, sox_mix, sox_mix_power,
  sox_merge, sox_multiply, sox_default
} combine_method = sox_default;
static enum { sox_single, sox_multiple } output_method = sox_single;
#define is_serial(m) ((m) <= sox_concatenate)
#define is_parallel(m) (!is_serial(m))
static sox_bool no_clobber = sox_false, interactive = sox_false;
static sox_bool uservolume = sox_false;
typedef enum {RG_off, RG_track, RG_album, RG_default} rg_mode;
static lsx_enum_item const rg_modes[] = {
  LSX_ENUM_ITEM(RG_,off)
  LSX_ENUM_ITEM(RG_,track)
  LSX_ENUM_ITEM(RG_,album)
  {0, 0}};
static rg_mode replay_gain_mode = RG_default;
static sox_option_t show_progress = sox_option_default;


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
  rg_mode replay_gain_mode;
} file_t;

static file_t * * files = NULL; /* Array tracking input and output files */
#define ofile files[file_count - 1]
static size_t file_count = 0;
static size_t input_count = 0;
static size_t output_count = 0;

/* Effects */

/* We parse effects into a temporary effects table and then place into
 * the real effects chain.  This allows scanning all effects to give
 * hints to what input effect options should be as well as determining
 * when mixer or resample effects need to be auto-inserted as well.
 */
static sox_effect_t **user_efftab = NULL;
static size_t user_efftab_size = 0;
static sox_effects_chain_t *effects_chain = NULL;
static sox_effect_t *save_output_eff = NULL;

static struct { char *name; int argc; char **argv; size_t argv_size; } **user_effargs = NULL;
static size_t *user_effargs_size = NULL;  /* array: size of user_effargs for each chain */
/* Size of memory structures related to effects arguments (user_effargs[i],
 * user_effargs[i][j].argv) to be extended in steps of EFFARGS_STEP */
#define EFFARGS_STEP 8
static size_t *nuser_effects = NULL;  /* array: number of effects in each chain */
static size_t current_eff_chain = 0;
static size_t eff_chain_count = 0;
static sox_bool very_first_effchain = sox_true;
  /* Indicates that not only the first effects chain is in effect (hrm), but
     also that it has never been restarted. Only then we may use the
     optimize_trim() hack. */
static char *effects_filename = NULL;
static char * play_rate_arg = NULL;
static char *norm_level = NULL;

/* Flowing */

static sox_signalinfo_t combiner_signal, ofile_signal_options;
static sox_encodinginfo_t combiner_encoding, ofile_encoding_options;
static uint64_t mixing_clips = 0;
static size_t current_input = 0;
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
static sox_sample_t omax[2], omin[2];

#include <termios.h>
static struct termios original_termios;
static sox_bool original_termios_saved = sox_false;

static sox_bool stdin_is_a_tty, is_player, is_guarded, do_guarded_norm, no_dither, reported_sox_opts;

struct timeval load_timeofday;

#define DEBUG(fmt,...) do{fprintf(stderr,"%s:%d " fmt "\n",__FILE__,__LINE__,##__VA_ARGS__);}while(0)

static void cleanup(void)
{
  size_t i;

  if (!success && !reported_sox_opts) {
    char const * env_opts = getenv(SOX_OPTS);
    if (env_opts && *env_opts)
      lsx_report("used "SOX_OPTS"=%s", env_opts);
  }
  /* Close the input and output files before exiting. */
  for (i = 0; i < input_count; i++) {
    if (files[i]->ft) {
      sox_close(files[i]->ft);
    }
    free(files[i]->filename);
    free(files[i]);
  }

  if (file_count) {
    if (ofile->ft) {
      if (!success && ofile->ft->io_type == lsx_io_file) {   /* If we failed part way through */
        struct stat st;                  /* writing a normal file, remove it. */
        if (!stat(ofile->ft->filename, &st) &&
            (st.st_mode & S_IFMT) == S_IFREG)
          unlink(ofile->ft->filename);
      }
      sox_close(ofile->ft); /* Assume we can unlink a file before closing it. */
    }
    free(ofile->filename);
    free(ofile);
  }

  free(files);

  if (original_termios_saved)
    tcsetattr(fileno(stdin), TCSANOW, &original_termios);

  free(user_efftab);

  free(sox_globals.tmp_path);
  sox_globals.tmp_path = NULL;

  free(play_rate_arg);
  free(effects_filename);
  free(norm_level);

  sox_quit();

  cleanup_called = 1;
}

/* Cleanup atexit() function, hence always called. */
static void atexit_cleanup(void)
{
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
  read_wide_samples = 0;
  input_wide_samples = f->ft->signal.length / f->ft->signal.channels;
  if (f->volume == HUGE_VAL)
    f->volume = 1;
  if (f->replay_gain != HUGE_VAL)
    f->volume *= pow(10.0, f->replay_gain / 20);
  if (effp && f->volume != floor(f->volume))
    effp->out_signal.precision = SOX_SAMPLE_PRECISION;
  f->ft->sox_errno = errno = 0;
}

/* Read up to max `wide' samples.  A wide sample contains one sample per channel
 * from the input audio. */
static size_t sox_read_wide(sox_format_t * ft, sox_sample_t * buf, size_t max)
{
  size_t len = max / combiner_signal.channels;
  len = sox_read(ft, buf, len * ft->signal.channels) / ft->signal.channels;
  if (!len && ft->sox_errno)
    lsx_fail("`%s' %s: %s",
        ft->filename, ft->sox_errstr, sox_strerror(ft->sox_errno));
  return len;
}

static void balance_input(sox_sample_t * buf, size_t ws, file_t * f)
{
  size_t s = ws * f->ft->signal.channels;

  if (f->volume != 1) while (s--) {
    double d = f->volume * *buf;
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
  uint64_t ws;
  size_t i;

  if (is_serial(combine_method))
    progress_to_next_input_file(files[current_input], effp);
  else {
    ws = 0;
    z->ibuf = lsx_malloc(input_count * sizeof(*z->ibuf));
    for (i = 0; i < input_count; i++) {
      z->ibuf[i] = lsx_malloc(sox_globals.bufsiz * sizeof(sox_sample_t));
      progress_to_next_input_file(files[i], effp);
      ws = max(ws, input_wide_samples);
    }
    input_wide_samples = ws; /* Output length is that of longest input file. */
  }
  z->ilen = lsx_malloc(input_count * sizeof(*z->ilen));
  return SOX_SUCCESS;
}

static sox_bool can_segue(size_t i)
{
  return
    files[i]->ft->signal.channels == files[i - 1]->ft->signal.channels &&
    files[i]->ft->signal.rate     == files[i - 1]->ft->signal.rate;
}

static int combiner_drain(sox_effect_t *effp, sox_sample_t * obuf, size_t * osamp)
{
  input_combiner_t * z = (input_combiner_t *) effp->priv;
  size_t ws, s, i;
  size_t olen = 0;

  if (is_serial(combine_method)) {
    while (sox_true) {
      if (!user_skip)
        olen = sox_read_wide(files[current_input]->ft, obuf, *osamp);
      if (olen == 0) {   /* If EOF, go to the next input file. */
        if (++current_input < input_count) {
          if (combine_method == sox_sequence && !can_segue(current_input))
            break;
          progress_to_next_input_file(files[current_input], NULL);
          continue;
        }
      }
      balance_input(obuf, olen, files[current_input]);
      break;
    } /* while */
  } /* is_serial */ else { /* else is_parallel() */
    sox_sample_t * p = obuf;
    for (i = 0; i < input_count; ++i) {
      z->ilen[i] = sox_read_wide(files[i]->ft, z->ibuf[i], *osamp);
      balance_input(z->ibuf[i], z->ilen[i], files[i]);
      olen = max(olen, z->ilen[i]);
    }
    for (ws = 0; ws < olen; ++ws) { /* wide samples */
      if (combine_method == sox_mix || combine_method == sox_mix_power) {
        for (s = 0; s < effp->in_signal.channels; ++s, ++p) { /* sum samples */
          *p = 0;
          for (i = 0; i < input_count; ++i)
            if (ws < z->ilen[i] && s < files[i]->ft->signal.channels) {
              /* Cast to double prevents integer overflow */
              double sample = *p + (double)z->ibuf[i][ws * files[i]->ft->signal.channels + s];
              *p = SOX_ROUND_CLIP_COUNT(sample, mixing_clips);
            }
        }
      } /* sox_mix */ else if (combine_method == sox_multiply)  {
        for (s = 0; s < effp->in_signal.channels; ++s, ++p) { /* multiply samples */
          i = 0;
          *p = ws < z->ilen[i] && s < files[i]->ft->signal.channels?
            z->ibuf[i][ws * files[i]->ft->signal.channels + s] : 0;
          for (++i; i < input_count; ++i) {
            double sample = *p * (-1. / SOX_SAMPLE_MIN) * (ws < z->ilen[i] && s < files[i]->ft->signal.channels? z->ibuf[i][ws * files[i]->ft->signal.channels + s] : 0);
            *p = SOX_ROUND_CLIP_COUNT(sample, mixing_clips);
          }
        }
      } /* sox_multiply */ else { /* sox_merge: like a multi-track recorder */
        for (i = 0; i < input_count; ++i)
          for (s = 0; s < files[i]->ft->signal.channels; ++s)
            *p++ = (ws < z->ilen[i]) * z->ibuf[i][ws * files[i]->ft->signal.channels + s];
      } /* sox_merge */
    } /* wide samples */
  } /* is_parallel */
  read_wide_samples += olen;
  olen *= effp->in_signal.channels;
  *osamp = olen;

  input_eof = olen ? sox_false : sox_true;

  if (input_eof && is_parallel(combine_method))
    current_input += input_count;

  return olen? SOX_SUCCESS : SOX_EOF;
}

static int combiner_stop(sox_effect_t *effp)
{
  input_combiner_t * z = (input_combiner_t *) effp->priv;
  size_t i;

  if (is_parallel(combine_method)) {
    /* Free input buffers now that they are not used */
    for (i = 0; i < input_count; i++)
      free(z->ibuf[i]);
    free(z->ibuf);
  }
  free(z->ilen);

  return SOX_SUCCESS;
}

static sox_effect_handler_t const * input_combiner_effect_fn(void)
{
  static sox_effect_handler_t handler = { "input", 0, SOX_EFF_MCHAN |
    SOX_EFF_MODIFY, 0, combiner_start, 0, combiner_drain,
    combiner_stop, 0, sizeof(input_combiner_t)
  };
  return &handler;
}

static int ostart(sox_effect_t *effp)
{
  unsigned prec = effp->out_signal.precision;
  if (effp->in_signal.mult && effp->in_signal.precision > prec)
    *effp->in_signal.mult *= 1 - (1 << (31 - prec)) * (1. / SOX_SAMPLE_MAX);
  return SOX_SUCCESS;
}

static int output_flow(sox_effect_t *effp, sox_sample_t const * ibuf,
    sox_sample_t * obuf, size_t * isamp, size_t * osamp)
{
  size_t len;

  (void)effp, (void)obuf;
  if (show_progress) for (len = 0; len < *isamp; len += effp->in_signal.channels) {
    omax[0] = max(omax[0], ibuf[len]);
    omin[0] = min(omin[0], ibuf[len]);
    if (effp->in_signal.channels > 1) {
      omax[1] = max(omax[1], ibuf[len + 1]);
      omin[1] = min(omin[1], ibuf[len + 1]);
    }
    else {
      omax[1] = omax[0];
      omin[1] = omin[0];
    }
  }
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
  return &handler;
}

static void auto_effect(sox_effects_chain_t *, char const *, int, char **,
    sox_signalinfo_t *, int *);

static int add_effect(sox_effects_chain_t * chain, sox_effect_t * effp,
    sox_signalinfo_t * in, sox_signalinfo_t const * out, int * guard) {
  int no_guard = -1;
  switch (*guard) {
    case 0: if (!(effp->handler.flags & SOX_EFF_GAIN)) {
      char * arg = "-h";
      auto_effect(chain, "gain", 1, &arg, in, &no_guard);
      ++*guard;
    }
    break;
    case 1: if (effp->handler.flags & SOX_EFF_GAIN) {
      char * arg = "-r";
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

  effp = sox_create_effect(sox_find_effect(name)); /* Should always succeed. */

  if (sox_effect_options(effp, argc, argv) == SOX_EOF)
    exit(1); /* The failing effect should have displayed an error message */

  if (add_effect(chain, effp, signal, &ofile->ft->signal, guard) != SOX_SUCCESS)
    exit(2); /* The effects chain should have displayed an error message */
  free(effp);
}

/* add_eff_chain() - NOTE: this only adds memory for one
 * additional effects chain beyond value of eff_chain_count.  It
 * does not unconditionally increase size of effects chain.
 */
static void add_eff_chain(void)
{
  lsx_revalloc(user_effargs, eff_chain_count+1);
  user_effargs[eff_chain_count] = lsx_malloc(sizeof(**user_effargs));

  lsx_revalloc(user_effargs_size, eff_chain_count+1);
  user_effargs_size[eff_chain_count] = 0;
  lsx_revalloc(nuser_effects, eff_chain_count+1);
  nuser_effects[eff_chain_count] = 0;
} /* add_eff_chain */

/* free_eff_chain() - the inverse of add_eff_chain().  Frees
 * one effects chain (with index eff_chain_count) such that
 * there are eff_chain_count left, the last having index
 * eff_chain_count-1.
 */
static void free_eff_chain(void)
{
  size_t j;
  int k;
  for (j = 0; j < nuser_effects[eff_chain_count]; j++)
  {
    free(user_effargs[eff_chain_count][j].name);
    user_effargs[eff_chain_count][j].name = NULL;
    for (k = 0; k < user_effargs[eff_chain_count][j].argc; k++)
    {
      free(user_effargs[eff_chain_count][j].argv[k]);
      user_effargs[eff_chain_count][j].argv[k] = NULL;
    }
    user_effargs[eff_chain_count][j].argc = 0;
    free(user_effargs[eff_chain_count][j].argv);
    user_effargs[eff_chain_count][j].argv = NULL;
    user_effargs[eff_chain_count][j].argv_size = 0;
  }
  nuser_effects[eff_chain_count] = 0;
  free(user_effargs[eff_chain_count]);
} /* free_eff_chain */

static void delete_eff_chains(void)
{
  while (eff_chain_count > 0) {
    eff_chain_count--;
    free_eff_chain();
  }
  free(user_effargs);
  free(user_effargs_size);
  free(nuser_effects);
  user_effargs = NULL;
  user_effargs_size = NULL;
  nuser_effects = NULL;
} /* delete_eff_chains */

static sox_bool is_pseudo_effect(const char *s)
{
  if (s)
  if (strcmp("newfile", s) == 0 ||
      strcmp("restart", s) == 0 ||
      strcmp(":", s) == 0)
    return sox_true;
  return sox_false;
} /* is_pseudo_effect */

static void parse_effects(int argc, char ** argv)
{
  while (optstate.ind < argc) {
    size_t eff_offset, j;
    int newline_mode = 0;

    eff_offset = nuser_effects[eff_chain_count];
    if (eff_offset == user_effargs_size[eff_chain_count]) {
      size_t i = user_effargs_size[eff_chain_count];
      user_effargs_size[eff_chain_count] += EFFARGS_STEP;
      lsx_revalloc(user_effargs[eff_chain_count], user_effargs_size[eff_chain_count]);
      for (; i < user_effargs_size[eff_chain_count]; i++) {
        user_effargs[eff_chain_count][i].argv = NULL;
        user_effargs[eff_chain_count][i].argv_size = 0;
      }
    }

    /* pseudo-effect ":" is used to create a new effects chain */
    if (strcmp(argv[optstate.ind], ":") == 0)
    {
      /* Only create a new chain if current one has effects.
       * Error checking will be done when loop is restarted.
       */
      if (nuser_effects[eff_chain_count] != 0)
      {
        eff_chain_count++;
        add_eff_chain();
      }
      optstate.ind++;
      continue;
    }

    if (strcmp(argv[optstate.ind], "newfile") == 0)
    {
      /* Start a new effect chain for newfile if user doesn't
       * manually do it.  Restart loop without advancing
       * optstate.ind to do error checking.
       */
      if (nuser_effects[eff_chain_count] != 0)
      {
        eff_chain_count++;
        add_eff_chain();
        continue;
      }
      newline_mode = 1;
      output_method = sox_multiple;
    }
    else if (strcmp(argv[optstate.ind], "restart") == 0)
    {
      /* Start a new effect chain for restart if user doesn't
       * manually do it.  Restart loop without advancing
       * optstate.ind to do error checking.
       */
      if (nuser_effects[eff_chain_count] != 0)
      {
        eff_chain_count++;
        add_eff_chain();
        continue;
      }
      newline_mode = 1;
    }

    /* Name should always be correct! */
    user_effargs[eff_chain_count][eff_offset].name = lsx_strdup(argv[optstate.ind]);
    optstate.ind++;
    for (j = 0; j < (size_t)(argc - optstate.ind) && !sox_find_effect(argv[optstate.ind + j]) &&
         !is_pseudo_effect(argv[optstate.ind + j]); ++j) {
      if (j >= user_effargs[eff_chain_count][eff_offset].argv_size) {
        user_effargs[eff_chain_count][eff_offset].argv_size += EFFARGS_STEP;
        lsx_revalloc(user_effargs[eff_chain_count][eff_offset].argv,
            user_effargs[eff_chain_count][eff_offset].argv_size);
      }
      user_effargs[eff_chain_count][eff_offset].argv[j] = lsx_strdup(argv[optstate.ind + j]);
    }
    user_effargs[eff_chain_count][eff_offset].argc = j;

    optstate.ind += j; /* Skip past the effect arguments */
    nuser_effects[eff_chain_count]++;
    if (newline_mode)
    {
      eff_chain_count++;
      add_eff_chain();
    }
  }
} /* parse_effects */

static char * * strtoargv(char * s, int * argc)
{
  sox_bool squote = sox_false;   /* Single quote mode (') is in effect. */
  sox_bool dquote = sox_false;   /* Double quote mode (") is in effect. */
  sox_bool esc    = sox_false;   /* Escape mode (\) is in effect. */
  char * t, * * argv = NULL;

  for (*argc = 0; *s;) {
    for (; isspace(*s); ++s);    /* Skip past any (more) white space. */
    if (*s) {                    /* Found an arg. */
      lsx_revalloc(argv, *argc + 1);
      argv[(*argc)++] = s;       /* Store pointer to start of arg. */
                                 /* Find the end of the arg: */
      for (t = s; *s && (esc || squote || dquote || !isspace(*s)); ++s)
        if (!esc && !squote && *s == '"')
          dquote = !dquote;      /* Toggle double quote mode. */
        else if (!esc && !dquote && *s == '\'')
          squote = !squote;      /* Toggle single quote mode. */
        else if (!(esc = !esc && *s == '\\' && s[1] &&
              (!squote && (s[1] == '"' || !dquote))))
          *t++ = *s;             /* Only copy if not an active ', ", or \ */
      s = *s ? s + 1 : s;        /* Skip the 1st white space char. */
      *t = '\0';                 /* Terminate the arg. */
    }
  }
  return argv;
}                                /* strtoargv */

static void read_user_effects(char const *filename)
{
    FILE *file = fopen(filename, "r");
    const size_t buffer_size_step = 1024;
    size_t buffer_size = buffer_size_step;
    char *s = lsx_malloc(buffer_size); /* buffer for one input line */
    int pos = 0;
    int argc;
    char * * argv;
    sox_bool last_was_colon = sox_false; /* last line read consisted of ":" only */

    /* Free any command line options and then re-initialize to
     * starter user_effargs.
     */
    delete_eff_chains();
    current_eff_chain = 0;
    add_eff_chain();

    if (!file) {
        lsx_fail("Cannot open effects file `%s': %s", filename, strerror(errno));
        exit(1);
    }

    lsx_report("Reading effects from file `%s'", filename);

    while(fgets(s + pos, (int) (buffer_size - pos), file)) {
      int len = strlen(s + pos);
      if (len && s[pos+len-1] == '\n')
        s[pos+len-1] = '\0', pos = 0; /* we've read a complete line */
      else if (len == (int)(buffer_size - pos - 1)) {
        /* line was longer than buffer size */
        buffer_size += buffer_size_step;
        s = lsx_realloc(s, buffer_size);
        pos += len;
        continue; /* read next part */
      } else {
        /* something strange happened; the file might have ended
           without a '\n', might contain '\0', or a read error
           occurred */
        if (ferror(file))
          break; /* use error reporting after loop */
        lsx_fail("Error reading effects file `%s' (not a text file?)", filename);
        exit(1);
      }

      last_was_colon = sox_false;

      argv = strtoargv(s, &argc);

      if (argv && argc == 1 && strcmp(argv[0], ":") == 0)
        last_was_colon = sox_true;

      if (argv) {
        /* Make sure first option is an effect name. */
        if (!sox_find_effect(argv[0]) && !is_pseudo_effect(argv[0]))
        {
          lsx_fail("Cannot find an effect called `%s'.", argv[0]);
          exit(1);
        }

        /* parse_effects normally parses options from command line.
         * Reset opt index so it thinks its back at beginning of
         * main()'s argv[].
         */
        optstate.ind = 0;
        parse_effects(argc, argv);

        /* Advance to next effect but only if current chain has been
         * filled in.  This recovers from side affects of pseudo-effects.
         */
        if (nuser_effects[eff_chain_count] > 0) {
          eff_chain_count++;
          add_eff_chain();
        }

        free(argv);
      }
    }
    if (ferror(file)) {
      lsx_fail("Error reading effects file `%s': %s", filename, strerror(errno));
      exit(1);
    }
    fclose(file);
    free(s);

    if (last_was_colon || eff_chain_count == 0) {
      /* user explicitly wanted an empty last effects chain,
         or didn't specify any chains at all */
      eff_chain_count++;
    } else {
      /* there's one unneeded effects chain */
      free_eff_chain();
    }
} /* read_user_effects */

/* Creates users effects and passes in user specified options.
 * This is done without putting anything into the effects chain
 * because an effect may set the effp->in_format and we may want
 * to copy that back into the input/combiner before opening and
 * inserting it.
 * Similarly, we may want to use effp->out_format to override the
 * default values of output file before we open it.
 * To keep things simple, we create all user effects.  Later, when
 * we add them, some may already be in the chain and we will need to free
 * them.
 */
static void create_user_effects(void)
{
  size_t i;
  sox_effect_t *effp;
  size_t num_effects = nuser_effects[current_eff_chain];

  DEBUG("num_effects: %lu", num_effects);
  /* extend user_efftab, if needed */
  if (user_efftab_size < num_effects) {
    user_efftab_size = num_effects;
    lsx_revalloc(user_efftab, num_effects);
  }

  for (i = 0; i < num_effects; i++) {
    DEBUG("effect: %s", user_effargs[current_eff_chain][i].name);
    effp = sox_create_effect(sox_find_effect(user_effargs[current_eff_chain][i].name));

    if (effp->handler.flags & SOX_EFF_DEPRECATED)
      lsx_warn("effect `%s' is deprecated; see sox(1) for an alternative",
          effp->handler.name);
    else if (effp->handler.flags & SOX_EFF_ALPHA)
      lsx_warn("effect `%s' is experimental/incomplete", effp->handler.name);
    else if (effp->handler.flags & SOX_EFF_INTERNAL) {
      lsx_fail("`%s' is a libSoX-only effect", effp->handler.name);
      exit(1);
    }

    /* The failing effect should have displayed an error message */
    if (sox_effect_options(effp, user_effargs[current_eff_chain][i].argc,
          user_effargs[current_eff_chain][i].argv) == SOX_EOF)
      exit(1);

    user_efftab[i] = effp;
  }
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
  size_t i;
  sox_effect_t * effp;
  char * rate_arg = is_player ? (play_rate_arg ? play_rate_arg : "-l") : NULL;

  /* 1st `effect' in the chain is the input combiner_signal.
   * add it only if its not there from a previous run.  */
  if (chain->length == 0) {
    effp = sox_create_effect(input_combiner_effect_fn());
    sox_add_effect(chain, effp, &signal, &ofile->ft->signal);
    free(effp);
  }

  /* Add user specified effects; stop before `dither' */
  for (i = 0; i < nuser_effects[current_eff_chain] &&
      strcmp(user_efftab[i]->handler.name, "dither"); i++) {
    if (add_effect(chain, user_efftab[i], &signal, &ofile->ft->signal,
          &guard) != SOX_SUCCESS)
      exit(2); /* Effects chain should have displayed an error message */
    free(user_efftab[i]);
  }

  /* Add auto effects if still needed at this point */
  if (signal.channels < ofile->ft->signal.channels &&
      signal.rate != ofile->ft->signal.rate)
    auto_effect(chain, "rate", rate_arg != NULL, &rate_arg, &signal, &guard);
  if (signal.channels != ofile->ft->signal.channels)
    auto_effect(chain, "channels", 0, NULL, &signal, &guard);
  if (signal.rate != ofile->ft->signal.rate)
    auto_effect(chain, "rate", rate_arg != NULL, &rate_arg, &signal, &guard);

  if (is_guarded && (do_guarded_norm || !(signal.mult && *signal.mult == 1))) {
    char *args[2];
    int no_guard = -1;
    args[0] = do_guarded_norm? "-nh" : guard? "-rh" : "-h";
    args[1] = norm_level;
    auto_effect(chain, "gain", norm_level ? 2 : 1, args, &signal, &no_guard);
    guard = 1;
  }

  if (i == nuser_effects[current_eff_chain] && !no_dither && signal.precision >
      ofile->ft->signal.precision && ofile->ft->signal.precision < 24)
    auto_effect(chain, "dither", 0, NULL, &signal, &guard);

  /* Add user specified effects from `dither' onwards */
  for (; i < nuser_effects[current_eff_chain]; i++, guard = 2) {
    if (add_effect(chain, user_efftab[i], &signal, &ofile->ft->signal,
          &guard) != SOX_SUCCESS)
      exit(2); /* Effects chain should have displayed an error message */
    free(user_efftab[i]);
  }

  if (!save_output_eff)
  {
    /* Last `effect' in the chain is the output file */
    effp = sox_create_effect(output_effect_fn());
    if (sox_add_effect(chain, effp, &signal, &ofile->ft->signal) != SOX_SUCCESS)
      exit(2);
    free(effp);
  }
  else
  {
    sox_push_effect_last(chain, save_output_eff);
    save_output_eff = NULL;
  }
}

static int advance_eff_chain(void)
{
  sox_bool reuse_output = sox_true;

  very_first_effchain = sox_false;

  /* If input file reached EOF then delete all effects in current
   * chain and restart the current chain.
   *
   * This is only used with sox_sequence combine mode even though
   * we do not specifically check for that method.
   */
  if (input_eof)
    sox_delete_effects(effects_chain);
  else
  {
    /* If user requested to restart this effect chain then
     * do not advance to next.  Usually, this is because
     * an option to current effect was changed.
     */
    if (user_restart_eff)
      user_restart_eff = sox_false;
    /* Effect chain stopped so advance to next effect chain but
     * quite if no more chains exist.
     */
    else if (++current_eff_chain >= eff_chain_count)
      return SOX_EOF;

    while (nuser_effects[current_eff_chain] == 1 &&
           is_pseudo_effect(user_effargs[current_eff_chain][0].name))
    {
      if (strcmp("newfile", user_effargs[current_eff_chain][0].name) == 0)
      {
        if (++current_eff_chain >= eff_chain_count)
          return SOX_EOF;
        reuse_output = sox_false;
      }
      else if (strcmp("restart", user_effargs[current_eff_chain][0].name) == 0)
        current_eff_chain = 0;
    }

    if (reuse_output)
      save_output_eff = sox_pop_effect_last(effects_chain);

    while (effects_chain->length > 1)
      sox_delete_effect_last(effects_chain);
  }
  return SOX_SUCCESS;
} /* advance_eff_chain */

static sox_bool since(struct timeval * then, double secs, sox_bool always_reset)
{
  sox_bool ret;
  struct timeval now;
  time_t d;
  gettimeofday(&now, NULL);
  d = now.tv_sec - then->tv_sec;
  ret = d > ceil(secs) || now.tv_usec - then->tv_usec + d * TIME_FRAC >= secs * TIME_FRAC;
  if (ret || always_reset)
    *then = now;
  return ret;
}

static int update_status(sox_bool all_done, void * client_data)
{
  return (user_abort || user_restart_eff) ? SOX_EOF : SOX_SUCCESS;
}

static void optimize_trim(void)
{
  /* Speed hack.  If the "trim" effect is the first effect then peek inside its
   * "effect descriptor" and see what the start location is.  This has to be
   * done after its start() is called to have the correct location.  Also, only
   * do this when only working with one input file.  This is because the logic
   * to do it for multiple files is complex and probably never used.  The same
   * is true for a restarted or additional effects chain (relative positioning
   * within the file and possible samples still buffered in the input effect
   * would have to be taken into account).  This hack is a huge time savings
   * when trimming gigs of audio data into managable chunks.  */
  if (input_count == 1 && very_first_effchain && effects_chain->length > 1 &&
      strcmp(effects_chain->effects[1][0].handler.name, "trim") == 0) {
    if (files[0]->ft->handler.seek && files[0]->ft->seekable){
      uint64_t offset = sox_trim_get_start(&effects_chain->effects[1][0]);
      if (offset && sox_seek(files[0]->ft, offset, SOX_SEEK_SET) == SOX_SUCCESS) {
        read_wide_samples = offset / files[0]->ft->signal.channels;
        /* Assuming a failed seek stayed where it was.  If the seek worked then
         * reset the start location of trim so that it thinks user didn't
         * request a skip.  */
        sox_trim_clear_start(&effects_chain->effects[1][0]);
        lsx_debug("optimize_trim successful");
      }
    }
  }
}

static sox_bool overwrite_permitted(char const * filename)
{
  char c;

  if (!no_clobber) {
    lsx_report("Overwriting `%s'", filename);
    return sox_true;
  }
  lsx_warn("Output file `%s' already exists", filename);
  if (!stdin_is_a_tty)
    return sox_false;
  do fprintf(stderr, "%s sox: overwrite `%s' (y/n)? ", myname, filename);
  while (scanf(" %c%*[^\n]", &c) != 1 || !strchr("yYnN", c));
  return c == 'y' || c == 'Y';
}

static char *fndup_with_count(const char *filename, size_t count)
{
    char *expand_fn, *efn;
    const char *fn, *ext, *end;
    sox_bool found_marker = sox_false;

    fn = filename;

    efn = expand_fn = lsx_malloc((size_t)FILENAME_MAX);

    /* Find extension in case user didn't specify a substitution
     * marker.
     */
    end = ext = filename + strlen(filename);
    while (ext > filename && *ext != '.')
        ext--;

    /* In case extension not found, point back to end of string to do less
     * copying later.
     */
    if (*ext != '.')
        ext = end;

    while (fn < end)
    {
        /* Look for %n. If found, replace with count.  Can specify an
         * option width of 1-9.
         */
        if (*fn == '%')
        {
            char width = 0;
            fn++;
            if (*fn >= '1' && *fn <= '9')
            {
                width = *fn++;
            }
            if (*fn == 'n')
            {
                char format[5];

                found_marker = sox_true;

                if (width)
                {
					sprintf(format, "%%0%cd", width);
                }
				else
				{
                    strcpy(format, "%02d");
				}

                efn += sprintf(efn, format, count);
                fn++;
            }
            else
                *efn++ = *fn++;
        }
        else
            *efn++ = *fn++;
    }

    *efn = 0;

    /* If user didn't tell us what to do then default to putting
     * the count right before file extension.
     */
    if (!found_marker)
    {
        efn -= strlen (ext);

        sprintf(efn, "%03lu", (unsigned long)count);
        efn = efn + 3;
        strcat(efn, ext);
    }

    return expand_fn;
}

static void open_output_file(void)
{
  double factor;
  int i;
  sox_comments_t p = ofile->oob.comments;
  sox_oob_t oob = files[0]->ft->oob;
  char *expand_fn;

  /* Skip opening file if we are not recreating output effect */
  if (save_output_eff)
    return;

  oob.comments = sox_copy_comments(files[0]->ft->oob.comments);

  if (!oob.comments && !p)
    sox_append_comment(&oob.comments, "Processed by SoX");
  else if (p) {
    if (!(*p)[0]) {
      sox_delete_comments(&oob.comments);
      ++p;
    }
    while (*p)
      sox_append_comment(&oob.comments, *p++);
  }

  /* Copy loop info, resizing appropriately it's in samples, so # channels
   * don't matter FIXME: This doesn't work for multi-file processing or effects
   * that change file length.  */
  factor = (double) ofile->signal.rate / combiner_signal.rate;
  for (i = 0; i < SOX_MAX_NLOOPS; i++) {
    oob.loops[i].start = oob.loops[i].start * factor;
    oob.loops[i].length = oob.loops[i].length * factor;
  }

  if (output_method == sox_multiple)
    expand_fn = fndup_with_count(ofile->filename, ++output_count);
  else
    expand_fn = lsx_strdup(ofile->filename);
  ofile->ft = sox_open_write(expand_fn, &ofile->signal, &ofile->encoding,
      ofile->filetype, &oob, overwrite_permitted);
  sox_delete_comments(&oob.comments);
  free(expand_fn);

  if (!ofile->ft)
    /* sox_open_write() will call lsx_warn for most errors.
     * Rely on that printing something. */
    exit(2);

  /* If whether to enable the progress display (similar to that of ogg123) has
   * not been specified by the user, auto turn on when outputting to an audio
   * device: */
  if (show_progress == sox_option_default)
    show_progress = (ofile->ft->handler.flags & SOX_FILE_DEVICE) != 0 &&
                    (ofile->ft->handler.flags & SOX_FILE_PHONY) == 0;
}

static void sigint(int s)
{
  static struct timeval then;
  if (input_count > 1 && show_progress && s == SIGINT &&
      is_serial(combine_method) && since(&then, 1.0, sox_true))
  {
    signal(SIGINT, sigint);
    user_skip = sox_true;
  }
  else user_abort = sox_true;
}

static void calculate_combiner_signal_parameters(void)
{
  size_t i;

  /* If user didn't specify # of channels then see if an effect
   * is specifying them.  This is of most use currently with the
   * synth effect were user can use null input handler and specify
   * channel counts directly in effect.  Forcing to use -c with
   * -n isn't as convenient.
   */
  DEBUG("input_count: %lu, current_eff_chain: %lu", input_count, current_eff_chain);
  for (i = 0; i < input_count; i++) {
    size_t j;
    for (j =0; j < nuser_effects[current_eff_chain] &&
               !files[i]->ft->signal.channels; ++j)
      files[i]->ft->signal.channels = user_efftab[j]->in_signal.channels;
    /* For historical reasons, default to one channel if not specified. */
    if (!files[i]->ft->signal.channels)
      files[i]->ft->signal.channels = 1;
  }

  /* Set the combiner output signal attributes to those of the 1st/next input
   * file.  If we are in sox_sequence mode then we don't need to check the
   * attributes of the other inputs, otherwise, it is mandatory that all input
   * files have the same sample rate, and for sox_concatenate, it is mandatory
   * that they have the same number of channels, otherwise, the number of
   * channels at the output of the combiner is calculated according to the
   * combiner mode. */
  combiner_signal = files[current_input]->ft->signal;
  DEBUG("combine_method: %u", combine_method);
  if (combine_method == sox_sequence) {
    /* Report all input files; do this only the 1st time process() is called: */
    combiner_signal.length = SOX_UNKNOWN_LEN;
  } else {
    size_t total_channels = 0;
    size_t min_channels = SOX_SIZE_MAX;
    size_t max_channels = 0;
    size_t min_rate = SOX_SIZE_MAX;
    size_t max_rate = 0;
    uint64_t total_length = 0, max_length_ws = 0;

    /* Report all input files and gather info on differing rates & numbers of
     * channels, and on the resulting output audio length: */
    for (i = 0; i < input_count; i++) {
      total_channels += files[i]->ft->signal.channels;
      min_channels = min(min_channels, files[i]->ft->signal.channels);
      max_channels = max(max_channels, files[i]->ft->signal.channels);
      min_rate     = min(min_rate    , files[i]->ft->signal.rate);
      max_rate     = max(max_rate    , files[i]->ft->signal.rate);
      max_length_ws = files[i]->ft->signal.length ?
          max(max_length_ws, files[i]->ft->signal.length / files[i]->ft->signal.channels) :
          SOX_UNKNOWN_LEN;
      if (total_length != SOX_UNKNOWN_LEN && files[i]->ft->signal.length)
        total_length += files[i]->ft->signal.length;
      else
        total_length = SOX_UNKNOWN_LEN;
    }

    /* Check for invalid/unusual rate or channel combinations: */
    if (min_rate != max_rate)
      lsx_fail("Input files must have the same sample-rate");
      /* Don't exit quite yet; give the user any other message 1st */
    if (min_channels != max_channels) {
      if (combine_method == sox_concatenate) {
        lsx_fail("Input files must have the same # channels");
        exit(1);
      } else if (combine_method != sox_merge)
        lsx_warn("Input files don't have the same # channels");
    }
    if (min_rate != max_rate)
      exit(1);

    /* Store the calculated # of combined channels: */
    combiner_signal.channels =
      combine_method == sox_merge? total_channels : max_channels;
	DEBUG("total_channels: %lu, max_channels: %lu, combiner_signal.channels: %u", total_channels, max_channels, combiner_signal.channels);

    if (combine_method == sox_concatenate)
      combiner_signal.length = total_length;
    else if (is_parallel(combine_method))
      combiner_signal.length = max_length_ws != SOX_UNKNOWN_LEN ?
          max_length_ws * combiner_signal.channels : SOX_UNKNOWN_LEN;
	DEBUG("combiner_signal.length: %lu", combiner_signal.length);
  }
} /* calculate_combiner_signal_parameters */

static void calculate_output_signal_parameters(void)
{
  sox_bool known_length = combine_method != sox_sequence;
  size_t i;
  uint64_t olen = 0;

  /* Report all input files and gather info on differing rates & numbers of
   * channels, and on the resulting output audio length: */
  for (i = 0; i < input_count; i++) {
    known_length = known_length && files[i]->ft->signal.length != SOX_UNSPEC;
    if (combine_method == sox_concatenate)
      olen += files[i]->ft->signal.length / files[i]->ft->signal.channels;
    else
      olen = max(olen, files[i]->ft->signal.length / files[i]->ft->signal.channels);
  }

  /* Determine the output file signal attributes; set from user options
   * if given: */
  ofile->signal = ofile_signal_options;

  /* If no user option for output rate or # of channels, set from the last
   * effect that sets these, or from the input combiner if there is none such */
  for (i = 0; i < nuser_effects[current_eff_chain] && !ofile->signal.rate; ++i)
    ofile->signal.rate = user_efftab[nuser_effects[current_eff_chain] - 1 - i]->out_signal.rate;
  for (i = 0; i < nuser_effects[current_eff_chain] && !ofile->signal.channels; ++i)
    ofile->signal.channels = user_efftab[nuser_effects[current_eff_chain] - 1 - i]->out_signal.channels;
  if (!ofile->signal.rate)
    ofile->signal.rate = combiner_signal.rate;
  if (!ofile->signal.channels)
    ofile->signal.channels = combiner_signal.channels;

  /* FIXME: comment this: */
  ofile->signal.precision = combiner_signal.precision;

  /* If any given user effect modifies the audio length, then we assume that
   * we don't know what the output length will be.  FIXME: in most cases,
   * an effect that modifies length will be able to determine by how much from
   * its getopts parameters, so olen should be calculable. */
  for (i = 0; i < nuser_effects[current_eff_chain]; i++)
    known_length = known_length && !(user_efftab[i]->handler.flags & SOX_EFF_LENGTH);

  if (!known_length)
    olen = 0;
  ofile->signal.length = (uint64_t)(olen * ofile->signal.channels * ofile->signal.rate / combiner_signal.rate + .5);
}

static void set_combiner_and_output_encoding_parameters(void)
{
  /* The input encoding parameters passed to the effects chain are those of
   * the first input file (for each segued block if sox_sequence):*/
  combiner_encoding = files[current_input]->ft->encoding;

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

  create_user_effects();

  calculate_combiner_signal_parameters();
  set_combiner_and_output_encoding_parameters();
  calculate_output_signal_parameters();
  open_output_file();

  if (!effects_chain)
    effects_chain = sox_create_effects_chain(&combiner_encoding,
                                             &ofile->ft->encoding);
  add_effects(effects_chain);

  if (very_first_effchain)
    optimize_trim();

  if (stdin_is_a_tty) {
    if (show_progress && is_player && !interactive) {
      lsx_debug("automatically entering interactive mode");
      interactive = sox_true;
    }
  } else if (interactive) {
    /* User called for interactive mode, but ... */
    lsx_warn("Standard input has to be a terminal for interactive mode");
    interactive = sox_false;
  }
  /* Prepare terminal for interactive mode and save the original termios
     settings. Do this only once, otherwise the "original" settings won't
     be original anymore after a second call to process() (next/restarted
     effects chain). */
  if (interactive && !original_termios_saved) {
    struct termios modified_termios;

    original_termios_saved = sox_true;
    tcgetattr(fileno(stdin), &original_termios);
    modified_termios = original_termios;
    modified_termios.c_lflag &= ~(ICANON | ECHO);
    modified_termios.c_cc[VMIN] = modified_termios.c_cc[VTIME] = 0;
    tcsetattr(fileno(stdin), TCSANOW, &modified_termios);
  }

  signal(SIGTERM, sigint); /* Stop gracefully, as soon as we possibly can. */
  signal(SIGINT , sigint); /* Either skip current input or behave as SIGTERM. */
  if (very_first_effchain) {
    struct timeval now;
    double d;
    gettimeofday(&now, NULL);
    d = now.tv_sec - load_timeofday.tv_sec + (now.tv_usec - load_timeofday.tv_usec) / TIME_FRAC;
    lsx_debug("start-up time = %g", d);
  }
  flow_status = sox_flow_effects(effects_chain, update_status, NULL);

  /* Don't return SOX_EOF if
   * 1) input reach EOF and there are more input files to process or
   * 2) output didn't return EOF (disk full?) there are more
   *    effect chains.
   * For case #2, something else must decide when to stop processing.
   */
  if ((input_eof && current_input < input_count) ||
      (!output_eof && current_eff_chain < eff_chain_count))
    flow_status = SOX_SUCCESS;

  return flow_status;
}

static char const * const getoptstr =
  "+b:c:de:hmnpqr:t:v:xBC:DGLMNRSTV::X";

static struct lsx_option_t const long_options[] = {
  {"add-comment"     , lsx_option_arg_required, NULL, 0},
  {"buffer"          , lsx_option_arg_required, NULL, 0},
  {"combine"         , lsx_option_arg_required, NULL, 0},
  {"comment-file"    , lsx_option_arg_required, NULL, 0},
  {"comment"         , lsx_option_arg_required, NULL, 0},
  {"endian"          , lsx_option_arg_required, NULL, 0},
  {"input-buffer"    , lsx_option_arg_required, NULL, 0},
  {"interactive"     , lsx_option_arg_none    , NULL, 0},
  {"help-effect"     , lsx_option_arg_required, NULL, 0},
  {"help-format"     , lsx_option_arg_required, NULL, 0},
  {"no-glob"         , lsx_option_arg_none    , NULL, 0},
  {"plot"            , lsx_option_arg_required, NULL, 0},
  {"replay-gain"     , lsx_option_arg_required, NULL, 0},
  {"version"         , lsx_option_arg_none    , NULL, 0},
  {"output"          , lsx_option_arg_required, NULL, 0},
  {"effects-file"    , lsx_option_arg_required, NULL, 0},
  {"temp"            , lsx_option_arg_required, NULL, 0},
  {"single-threaded" , lsx_option_arg_none    , NULL, 0},
  {"ignore-length"   , lsx_option_arg_none    , NULL, 0},
  {"norm"            , lsx_option_arg_optional, NULL, 0},
  {"magic"           , lsx_option_arg_none    , NULL, 0},
  {"play-rate-arg"   , lsx_option_arg_required, NULL, 0},
  {"clobber"         , lsx_option_arg_none    , NULL, 0},
  {"no-clobber"      , lsx_option_arg_none    , NULL, 0},
  {"multi-threaded"  , lsx_option_arg_none    , NULL, 0},
  {"dft-min"         , lsx_option_arg_required, NULL, 0},

  {"bits"            , lsx_option_arg_required, NULL, 'b'},
  {"channels"        , lsx_option_arg_required, NULL, 'c'},
  {"compression"     , lsx_option_arg_required, NULL, 'C'},
  {"default-device"  , lsx_option_arg_none    , NULL, 'd'},
  {"no-dither"       , lsx_option_arg_none    , NULL, 'D'},
  {"encoding"        , lsx_option_arg_required, NULL, 'e'},
  {"help"            , lsx_option_arg_none    , NULL, 'h'},
  {"null"            , lsx_option_arg_none    , NULL, 'n'},
  {"no-show-progress", lsx_option_arg_none    , NULL, 'q'},
  {"pipe"            , lsx_option_arg_none    , NULL, 'p'},
  {"rate"            , lsx_option_arg_required, NULL, 'r'},
  {"reverse-bits"    , lsx_option_arg_none    , NULL, 'X'},
  {"reverse-nibbles" , lsx_option_arg_none    , NULL, 'N'},
  {"show-progress"   , lsx_option_arg_none    , NULL, 'S'},
  {"type"            , lsx_option_arg_required, NULL, 't'},
  {"volume"          , lsx_option_arg_required, NULL, 'v'},
  {"guard"           , lsx_option_arg_none    , NULL, 'G'},

  {NULL, 0, NULL, 0}
};

static lsx_enum_item const combine_methods[] = {
  LSX_ENUM_ITEM(sox_,sequence)
  LSX_ENUM_ITEM(sox_,concatenate)
  LSX_ENUM_ITEM(sox_,mix)
  {"mix-power", sox_mix_power},
  LSX_ENUM_ITEM(sox_,merge)
  LSX_ENUM_ITEM(sox_,multiply)
  {0, 0}};

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

static lsx_enum_item const encodings[] = {
  {"signed-integer", encoding_signed_integer},
  {"unsigned-integer", encoding_unsigned_integer},
  {"floating-point", encoding_floating_point},
  {"ms-adpcm", encoding_ms_adpcm},
  {"ima-adpcm", encoding_ima_adpcm},
  {"oki-adpcm", encoding_oki_adpcm},
  {"gsm-full-rate", encoding_gsm_full_rate},
  {"u-law", encoding_u_law},
  {"mu-law", encoding_u_law},
  {"a-law", encoding_a_law},
  {0, 0}};

static char const * device_name(char const * const type)
{
  char * name = NULL, * from_env = getenv("AUDIODEV");

  if (!type)
    return NULL;

  if (0
      || !strcmp(type, "sunau")
      || !strcmp(type, "oss" )
      || !strcmp(type, "ossdsp")
      || !strcmp(type, "alsa")
      || !strcmp(type, "ao")
      || !strcmp(type, "sndio")
      || !strcmp(type, "coreaudio")
      || !strcmp(type, "pulseaudio")
      || !strcmp(type, "waveaudio")
      )
    name = "default";
  
  return name? from_env? from_env : name : NULL;
}

static char const * try_device(char const * name)
{
  sox_format_handler_t const * handler = sox_find_format(name, sox_false);
  if (handler) {
    sox_format_t format, * ft = &format;
    lsx_debug("Looking for a default device: trying format `%s'", name);
    memset(ft, 0, sizeof(*ft));
    ft->filename = (char *)device_name(name);
    ft->priv = lsx_calloc(1, handler->priv_size);
    if (handler->startwrite(ft) == SOX_SUCCESS) {
      handler->stopwrite(ft);
      free(ft->priv);
      return name;
    }
    free(ft->priv);
  }
  return NULL;
}

static char const * set_default_device(file_t * f)
{
  /* Default audio driver type in order of preference: */
  if (!f->filetype) f->filetype = getenv("AUDIODRIVER");
  if (!f->filetype) f->filetype = try_device("coreaudio");
  if (!f->filetype) f->filetype = try_device("pulseaudio");
  if (!f->filetype) f->filetype = try_device("alsa");
  if (!f->filetype) f->filetype = try_device("waveaudio");
  if (!f->filetype) f->filetype = try_device("sndio");
  if (!f->filetype) f->filetype = try_device("oss");
  if (!f->filetype) f->filetype = try_device("sunau");
  if (!f->filetype && file_count) /*!rec*/
    f->filetype = try_device("ao");

  if (!f->filetype) {
    lsx_fail("Sorry, there is no default audio device configured");
    exit(1);
  }
  return device_name(f->filetype);
}

static int add_file(file_t const * const opts, char const * const filename)
{
  file_t * f = lsx_malloc(sizeof(*f));

  *f = *opts;
  DEBUG("filename: %s", filename);
  f->filename = lsx_strdup(filename);
  files = lsx_realloc(files, (file_count + 1) * sizeof(*files));
  files[file_count++] = f;
  return 0;
}

static int add_glob_file(file_t const * const opts, char const * const filename)
{
  glob_t globbuf;
  size_t i;

  if (glob(filename, GLOB_BRACE | GLOB_TILDE | GLOB_NOCHECK, NULL, &globbuf)) {
    lsx_fail("glob: %s", strerror(errno));
    exit(1);
  }
  for (i = 0; i < globbuf.gl_pathc; ++i) {
    add_file(opts, globbuf.gl_pathv[i]);
  }
  globfree(&globbuf);
  return 0;
}

static void init_file(file_t * f)
{
  memset(f, 0, sizeof(*f));
  sox_init_encodinginfo(&f->encoding);
  f->volume = HUGE_VAL;
  f->replay_gain = HUGE_VAL;
}

static void parse_options_and_filenames(int argc, char **argv)
{
  file_t opts, opts_none;
  init_file(&opts), init_file(&opts_none);

#if 0
  lsx_getopt_init(argc, argv, getoptstr, long_options, lsx_getopt_flag_opterr, 1, &optstate);
  for (; optstate.ind < argc && !sox_find_effect(argv[optstate.ind]); init_file(&opts)) {
    if (!sox_is_playlist(argv[optstate.ind])) {
      add_glob_file(&opts, argv[optstate.ind++]);
    }
  }
#else
  lsx_getopt_init(argc, argv, getoptstr, long_options, lsx_getopt_flag_opterr, 1, &optstate);
  if (!sox_is_playlist(argv[1])) {
    add_glob_file(&opts, argv[1]);
  }
  optstate.ind++;
  init_file(&opts);
#endif
  add_file(&opts, set_default_device(&opts));
}

static void set_replay_gain(sox_comments_t comments, file_t * f)
{
  rg_mode rg = replay_gain_mode;
  int try = 2; /* Will try to find the other GAIN if preferred one not found */
  size_t i, n = sox_num_comments(comments);

  if (rg != RG_off) while (try--) {
    char const * target =
      rg == RG_track? "REPLAYGAIN_TRACK_GAIN=" : "REPLAYGAIN_ALBUM_GAIN=";
    for (i = 0; i < n; ++i) {
      if (strncasecmp(comments[i], target, strlen(target)) == 0) {
        f->replay_gain = atof(comments[i] + strlen(target));
        f->replay_gain_mode = rg;
        return;
      }
    }
    rg ^= RG_track ^ RG_album;
  }
}

static void output_message(unsigned level, const char *filename, const char *fmt, va_list ap)
{
  char const * const str[] = {"FAIL", "WARN", "INFO", "DBUG"};
  if (sox_globals.verbosity >= level) {
    char base_name[128];
    sox_basename(base_name, sizeof(base_name), filename);
    fprintf(stderr, "%s %s %s: ", myname, str[min(level - 1, 3)], base_name);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
  }
}

static sox_bool cmp_comment_text(char const * c1, char const * c2)
{
  return c1 && c2 && !strcasecmp(c1, c2);
}

int main(int argc, char **argv)
{
  size_t i;

  gettimeofday(&load_timeofday, NULL);
  myname = argv[0];
  sox_globals.output_message_handler = output_message;

  sox_mode = sox_play;

  if (sox_init() != SOX_SUCCESS)
    exit(1);

  stdin_is_a_tty = isatty(fileno(stdin));
  errno = 0; /* Both isatty & fileno may set errno. */

  atexit(atexit_cleanup);

  parse_options_and_filenames(argc, argv);

  input_count = file_count ? file_count - 1 : 0;

  if (file_count) {
    sox_format_handler_t const * handler =
      sox_write_handler(ofile->filename, ofile->filetype, NULL);
    is_player = handler &&
      (handler->flags & SOX_FILE_DEVICE) && !(handler->flags & SOX_FILE_PHONY);
  }

  if (combine_method == sox_default)
    combine_method = is_player? sox_sequence : sox_concatenate;

  /* Allow e.g. known length processing in this case */
  if (combine_method == sox_sequence && input_count == 1)
    combine_method = sox_concatenate;

  signal(SIGINT, SIG_IGN); /* So child pipes aren't killed by track skip */
  for (i = 0; i < input_count; i++) {
    size_t j = input_count - 1 - i; /* Open in reverse order 'cos of rec (below) */
    file_t * f = files[j];

    /* When mixing audio, default to input side volume adjustments that will
     * make sure no clipping will occur.  Users probably won't be happy with
     * this, and will override it, possibly causing clipping to occur. */
    if (combine_method == sox_mix && !uservolume)
      f->volume = 1.0 / input_count;
    else if (combine_method == sox_mix_power && !uservolume)
      f->volume = 1.0 / sqrt((double)input_count);

    files[j]->ft = sox_open_read(f->filename, &f->signal, &f->encoding, f->filetype);
    if (!files[j]->ft)
      /* sox_open_read() will call lsx_warn for most errors.
       * Rely on that printing something. */
      exit(2);
    if (show_progress == sox_option_default &&
        (files[j]->ft->handler.flags & SOX_FILE_DEVICE) != 0 &&
        (files[j]->ft->handler.flags & SOX_FILE_PHONY) == 0)
      show_progress = sox_option_yes;
  }

  if (replay_gain_mode == RG_default)
    replay_gain_mode = is_player?
      input_count > 1 &&               /* Simple heuristic to determine if */
      cmp_comment_text(                /* replay-gain should be in album mode */
          sox_find_comment(files[0]->ft->oob.comments, "artist"),
          sox_find_comment(files[1]->ft->oob.comments, "artist")) &&
      cmp_comment_text(
          sox_find_comment(files[0]->ft->oob.comments, "album"),
          sox_find_comment(files[1]->ft->oob.comments, "album"))?
      RG_album : RG_track : RG_off;

  for (i = 0; i < input_count; i++)
    set_replay_gain(files[i]->ft->oob.comments, files[i]);

  signal(SIGINT, SIG_DFL);

  /* Loop through the rest of the arguments looking for effects */
  add_eff_chain();
  parse_effects(argc, argv);
  eff_chain_count++;
  /* Note: Purposely not calling add_eff_chain() to save some
   * memory although it would be more consistent to do so.
   */

  /* Not the best way for users to do this; now deprecated in favour of soxi. */
  if (!show_progress && !nuser_effects[current_eff_chain] &&
      ofile->filetype && !strcmp(ofile->filetype, "null")) {
    exit(0);
  }

  if (!sox_globals.repeatable) {/* Re-seed PRNG? */
    struct timeval now;
    gettimeofday(&now, NULL);
    sox_globals.ranqd1 = (int32_t)(now.tv_sec - now.tv_usec);
  }

  /* Save things that sox_sequence needs to be reinitialised for each segued
   * block of input files.*/
  ofile_signal_options = ofile->signal;
  ofile_encoding_options = ofile->encoding;

  /* If user specified an effects filename then use that file
   * to load user effects.  Free any previously specified options
   * from the command line.
   */
  if (effects_filename)
  {
    read_user_effects(effects_filename);
  }

  while (process() != SOX_EOF && !user_abort && current_input < input_count)
  {
    if (advance_eff_chain() == SOX_EOF)
      break;

    if (!save_output_eff)
    {
      sox_close(ofile->ft);
      ofile->ft = NULL;
    }
  }

  sox_delete_effects_chain(effects_chain);
  delete_eff_chains();

  for (i = 0; i < file_count; ++i)
    if (files[i]->ft->clips != 0)
      lsx_warn(i < input_count?"`%s' input clipped %" PRIu64 " samples" :
                              "`%s' output clipped %" PRIu64 " samples; decrease volume?",
          (files[i]->ft->handler.flags & SOX_FILE_DEVICE)?
                       files[i]->ft->handler.names[0] : files[i]->ft->filename,
          files[i]->ft->clips);

  if (mixing_clips > 0)
    lsx_warn("mix-combining clipped %" PRIu64 " samples; decrease volume?", mixing_clips);

  for (i = 0; i < file_count; i++)
    if (files[i]->volume_clips > 0)
      lsx_warn("`%s' balancing clipped %" PRIu64 " samples; decrease volume?",
          files[i]->filename, files[i]->volume_clips);

  if (show_progress) {
    if (user_abort)
      fprintf(stderr, "Aborted.\n");
    else
      fprintf(stderr, "Skipped.\n");
  }

  success = 1; /* Signal success to cleanup so the output file isn't removed. */

  cleanup();

  return 0;
}
