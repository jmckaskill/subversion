/*
 * opt.c :  option and argument parsing for Subversion command lines
 *
 * ====================================================================
 * Copyright (c) 2000-2003 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */



#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <apr_pools.h>
#include <apr_general.h>
#include <apr_lib.h>

#include "svn_version.h"
#include "svn_types.h"
#include "svn_wc.h"
#include "svn_opt.h"
#include "svn_error.h"
#include "svn_pools.h"
#include "svn_path.h"
#include "svn_utf.h"
#include "svn_time.h"


/*** Code. ***/

const svn_opt_subcommand_desc_t *
svn_opt_get_canonical_subcommand (const svn_opt_subcommand_desc_t *table,
                                  const char *cmd_name)
{
  int i = 0;

  if (cmd_name == NULL)
    return NULL;

  while (table[i].name) {
    int j;
    if (strcmp (cmd_name, table[i].name) == 0)
      return table + i;
    for (j = 0; (j < SVN_OPT_MAX_ALIASES) && table[i].aliases[j]; j++)
      if (strcmp (cmd_name, table[i].aliases[j]) == 0)
        return table + i;

    i++;
  }

  /* If we get here, there was no matching subcommand name or alias. */
  return NULL;
}


const apr_getopt_option_t *
svn_opt_get_option_from_code (int code,
                              const apr_getopt_option_t *option_table)
{
  apr_size_t i;

  for (i = 0; option_table[i].optch; i++)
    if (option_table[i].optch == code)
      return &(option_table[i]);
  
  return NULL;
}


svn_boolean_t
svn_opt_subcommand_takes_option (const svn_opt_subcommand_desc_t *command,
                                 int option_code)
{
  apr_size_t i;
  
  for (i = 0; i < SVN_OPT_MAX_OPTIONS; i++)
    if (command->valid_options[i] == option_code)
      return TRUE;

  return FALSE;
}


/* Print the canonical command name for CMD, and all its aliases, to
   STREAM.  If HELP is set, print CMD's help string too, in which case
   obtain option usage from OPTIONS_TABLE. */
static void
print_command_info (const svn_opt_subcommand_desc_t *cmd,
                    const apr_getopt_option_t *options_table,
                    svn_boolean_t help, 
                    apr_pool_t *pool,
                    FILE *stream)
{
  svn_boolean_t first_time;
  apr_size_t i;

  /* Print the canonical command name. */
  fputs (cmd->name, stream);

  /* Print the list of aliases. */
  first_time = TRUE;
  for (i = 0; i < SVN_OPT_MAX_ALIASES; i++) 
    {
      if (cmd->aliases[i] == NULL)
        break;

      if (first_time) {
        fprintf (stream, " (");
        first_time = FALSE;
      }
      else
        fprintf (stream, ", ");
      
      fprintf (stream, "%s", cmd->aliases[i]);
    }

  if (! first_time)
    fprintf (stream, ")");
  
  if (help)
    {
      const apr_getopt_option_t *option;
      svn_boolean_t have_options = FALSE;

      fprintf (stream, ": %s", cmd->help);

      /* Loop over all valid option codes attached to the subcommand */
      for (i = 0; i < SVN_OPT_MAX_OPTIONS; i++)
        {
          if (cmd->valid_options[i])
            {
              if (have_options == FALSE)
                {
                  fprintf (stream, "\nValid options:\n");
                  have_options = TRUE;
                }

              /* convert each option code into an option */
              option = 
                svn_opt_get_option_from_code (cmd->valid_options[i],
                                              options_table);

              /* print the option's docstring */
              if (option)
                {
                  const char *optstr;
                  svn_opt_format_option (&optstr, option, TRUE, pool);
                  fprintf (stream, "  %s\n", optstr);
                }
            }
        }

      if (have_options)
        fprintf (stream, "\n");
    }
}


void
svn_opt_print_generic_help (const char *header,
                            const svn_opt_subcommand_desc_t *cmd_table,
                            const apr_getopt_option_t *opt_table,
                            const char *footer,
                            apr_pool_t *pool, FILE *stream)
{
  int i = 0;

  if (header)
    fprintf (stream, "%s", header);
  
  while (cmd_table[i].name) 
    {
      fprintf (stream, "   ");
      print_command_info (cmd_table + i, opt_table, FALSE, pool, stream);
      fprintf (stream, "\n");
      i++;
    }

  fprintf (stream, "\n");
  
  if (footer)
    fprintf (stream, "%s", footer);
}


void
svn_opt_format_option (const char **string,
                       const apr_getopt_option_t *opt,
                       svn_boolean_t doc,
                       apr_pool_t *pool)
{
  char *opts;

  if (opt == NULL)
    {
      *string = "?";
      return;
    }

  /* We have a valid option which may or may not have a "short
     name" (a single-character alias for the long option). */
  if (opt->optch <= 255)  
    opts = apr_psprintf (pool, "-%c [--%s]", opt->optch, opt->name);
  else
    opts = apr_psprintf (pool, "--%s", opt->name);

  if (opt->has_arg)
    opts = apr_pstrcat (pool, opts, " arg", NULL);

  if (doc)
    opts = apr_psprintf (pool, "%-24s : %s", opts, opt->description);

  *string = opts;
}


void
svn_opt_subcommand_help (const char *subcommand,
                         const svn_opt_subcommand_desc_t *table,
                         const apr_getopt_option_t *options_table,
                         apr_pool_t *pool)
{
  const svn_opt_subcommand_desc_t *cmd =
    svn_opt_get_canonical_subcommand (table, subcommand);
    
  if (cmd)
    print_command_info (cmd, options_table, TRUE, pool, stdout);
  else
    fprintf (stderr, "\"%s\": unknown command.\n\n", subcommand);
}



/*** Parsing revision and date options. ***/


/** Parsing "X:Y"-style arguments. **/

/* If WORD matches one of the special revision descriptors,
 * case-insensitively, set *REVISION accordingly:
 *
 *   - For "head", set REVISION->kind to svn_opt_revision_head.
 *
 *   - For "first", set REVISION->kind to svn_opt_revision_number
 *     and REVISION->value.number to 0.  ### iffy, but might be useful
 *     when mixed with dates ###
 *
 *   - For "prev", set REVISION->kind to svn_opt_revision_previous.
 *
 *   - For "base", set REVISION->kind to svn_opt_revision_base.
 *
 *   - For "committed" or "changed", set REVISION->kind to
 *     svn_opt_revision_committed.
 *
 * If match, return 0, else return -1 and don't touch REVISION.
 *
 * ### should we enforce a requirement that users write out these
 * words in full?  Actually, we probably will need to start enforcing
 * it as date parsing gets more sophisticated and the chances of a
 * first-letter overlap between a valid date and a valid word go up.
 */
static int
revision_from_word (svn_opt_revision_t *revision, const char *word)
{
  if (strcasecmp (word, "head") == 0)
    {
      revision->kind = svn_opt_revision_head;
    }
  else if (strcasecmp (word, "first") == 0)
    {
      revision->kind = svn_opt_revision_number;
      revision->value.number = 0;
    }
  else if (strcasecmp (word, "prev") == 0)
    {
      revision->kind = svn_opt_revision_previous;
    }
  else if (strcasecmp (word, "base") == 0)
    {
      revision->kind = svn_opt_revision_base;
    }
  else if ((strcasecmp (word, "committed") == 0)
           || (strcasecmp (word, "changed") == 0))
    {
      revision->kind = svn_opt_revision_committed;
    }
  else
    return -1;

  return 0;
}


/* Parse one revision specification.  Return pointer to character
   after revision, or NULL if the revision is invalid.  Modifies
   str, so make sure to pass a copy of anything precious. */
static char *parse_one_rev (svn_opt_revision_t *revision, char *str)
{
  char *end, save;
  time_t tm;

  if (*str == '{')
    {
      /* Brackets denote a date. */
      str++;
      end = strchr (str, '}');
      if (!end)
        return NULL;
      *end = '\0';
      tm = svn_parse_date (str, NULL);
      if (tm == -1)
        return NULL;
      revision->kind = svn_opt_revision_date;
      apr_time_ansi_put (&(revision->value.date), tm);
      return end + 1;
    }
  else if (apr_isdigit (*str))
    {
      /* It's a number. */
      end = str + 1;
      while (apr_isdigit (*end))
        end++;
      save = *end;
      *end = '\0';
      revision->kind = svn_opt_revision_number;
      revision->value.number = SVN_STR_TO_REV (str);
      *end = save;
      return end;
    }
  else if (apr_isalpha (*str))
    {
      end = str + 1;
      while (apr_isalpha (*end))
        end++;
      save = *end;
      *end = '\0';
      if (revision_from_word (revision, str) != 0)
        return NULL;
      *end = save;
      return end;
    }
  else
    return NULL;
}


int
svn_opt_parse_revision (svn_opt_revision_t *start_revision,
                        svn_opt_revision_t *end_revision,
                        const char *arg,
                        apr_pool_t *pool)
{
  char *left_rev, *right_rev, *end;

  /* Operate on a copy of the argument. */
  left_rev = apr_pstrdup (pool, arg);

  right_rev = parse_one_rev (start_revision, left_rev);
  if (right_rev && *right_rev == ':')
    {
      right_rev++;
      end = parse_one_rev (end_revision, right_rev);
      if (!end || *end != '\0')
        return -1;
    }
  else if (!right_rev || *right_rev != '\0')
    return -1;

  return 0;
}



/*** Parsing arguments. ***/
#define DEFAULT_ARRAY_SIZE 5


/* Copy STR into POOL and push the copy onto ARRAY.
   ### todo: Hmm. This should probably find its way into libsvn_subr -Fitz */
static void 
array_push_str (apr_array_header_t *array,
                const char *str,
                apr_pool_t *pool)
{
  /* ### Not sure if this function is still necessary.  It used to
     convert str to svn_stringbuf_t * and push it, but now it just
     dups str in pool and pushes the copy.  So its only effect is
     transfer str's lifetime to pool.  Is that something callers are
     depending on? */

  (*((const char **) apr_array_push (array))) = apr_pstrdup (pool, str);
}


void
svn_opt_push_implicit_dot_target (apr_array_header_t *targets, 
                                  apr_pool_t *pool)
{
  if (targets->nelts == 0)
    array_push_str (targets, "", pool); /* Ha! "", not ".", is the canonical */
  assert (targets->nelts);
}


svn_error_t *
svn_opt_parse_num_args (apr_array_header_t **args_p,
                        apr_getopt_t *os,
                        int num_args,
                        apr_pool_t *pool)
{
  int i;
  apr_array_header_t *args 
    = apr_array_make (pool, DEFAULT_ARRAY_SIZE, sizeof (const char *));

  /* loop for num_args and add each arg to the args array */
  for (i = 0; i < num_args; i++)
    {
      if (os->ind >= os->argc)
        {
          return svn_error_create (SVN_ERR_CL_ARG_PARSING_ERROR, 
                                   0, "too few arguments");
        }
      array_push_str (args, os->argv[os->ind++], pool);
    }

  *args_p = args;
  return SVN_NO_ERROR;
}

svn_error_t *
svn_opt_parse_all_args (apr_array_header_t **args_p,
                        apr_getopt_t *os,
                        apr_pool_t *pool)
{
  apr_array_header_t *args 
    = apr_array_make (pool, DEFAULT_ARRAY_SIZE, sizeof (const char *));

  if (os->ind > os->argc)
    {
      return svn_error_create (SVN_ERR_CL_ARG_PARSING_ERROR, 0, "");
    }
  while (os->ind < os->argc)
    {
      array_push_str (args, os->argv[os->ind++], pool);
    }

  *args_p = args;
  return SVN_NO_ERROR;
}


/* Parse a working-copy or url PATH, looking for an "@" sign, e.g.

         foo/bar/baz@13
         http://blah/bloo@27
         blarg/snarf@HEAD

   If an "@" is found, return the two halves in *TRUEPATH and *REV,
   allocating in POOL.

   If no "@" is found, set *TRUEPATH to PATH and *REV to kind 'unspecified'.
*/
static svn_error_t *
parse_path (svn_opt_revision_t *rev,
            const char **truepath,
            const char *path /* UTF-8! */,
            apr_pool_t *pool)
{
  int i;
  apr_pool_t *subpool = svn_pool_create (pool);
  svn_opt_revision_t start_revision, end_revision;

  /* scanning from right to left, just to be friendly to any
     screwed-up filenames that might *actually* contain @-signs.  :-) */
  for (i = (strlen (path) - 1); i >= 0; i--)
    {
      if (path[i] == '@')
        {
          const char *native_rev;

          SVN_ERR (svn_utf_cstring_from_utf8 (&native_rev, path + i + 1,
                                              subpool));

          if (svn_opt_parse_revision (&start_revision,
                                      &end_revision,
                                      native_rev, subpool))
            return svn_error_createf (SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
                                      "Syntax error parsing revision \"%s\"",
                                      path + i + 1);

          *truepath = apr_pstrndup (pool, path, i);
          rev->kind = start_revision.kind;
          rev->value = start_revision.value;

          svn_pool_destroy (subpool);
          return SVN_NO_ERROR;
        }
    }

  /* Didn't find an @-sign. */
  *truepath = path;
  rev->kind = svn_opt_revision_unspecified;

  svn_pool_destroy (subpool);
  return SVN_NO_ERROR;
}


svn_error_t *
svn_opt_args_to_target_array (apr_array_header_t **targets_p, 
                              apr_getopt_t *os,
                              apr_array_header_t *known_targets,
                              svn_opt_revision_t *start_revision,
                              svn_opt_revision_t *end_revision,
                              svn_boolean_t extract_revisions,
                              apr_pool_t *pool)
{
  int i;
  apr_array_header_t *input_targets =
    apr_array_make (pool, DEFAULT_ARRAY_SIZE, sizeof (const char *));
  apr_array_header_t *output_targets =
    apr_array_make (pool, DEFAULT_ARRAY_SIZE, sizeof (const char *));

  /* Step 1:  create a master array of targets that are in UTF-8
     encoding, and come from concatenating the targets left by apr_getopt,
     plus any extra targets (e.g., from the --targets switch.) */

  for (; os->ind < os->argc; os->ind++)
    {
      /* The apr_getopt targets are still in native encoding. */
      const char *raw_target = os->argv[os->ind];
      SVN_ERR (svn_utf_cstring_to_utf8
               ((const char **) apr_array_push (input_targets),
                raw_target, NULL, pool));
    }

  if (known_targets)
    {
      for (i = 0; i < known_targets->nelts; i++)
        {
          /* The --targets array have already been converted to UTF-8,
             because we needed to split up the list with svn_cstring_split. */
          const char *utf8_target = APR_ARRAY_IDX(known_targets,
                                                  i, const char *);
          (*((const char **) apr_array_push (input_targets))) = utf8_target;
        }
    }

  /* Step 2:  process each target.  */

  for (i = 0; i < input_targets->nelts; i++)
    {
      const char *utf8_target = APR_ARRAY_IDX(input_targets, i, const char *);
      const char *target;      /* after all processing is finished */

      /* URLs and wc-paths get treated differently. */
      if (svn_path_is_url (utf8_target))
        {
          /* No need to canonicalize a URL's case or path separators. */

          /* strip any trailing '/' */
          target = svn_path_canonicalize (utf8_target, pool);
        }
      else  /* not a url, so treat as a path */
        {
          const char *apr_target;
          const char *base_name;
          char *truenamed_target; /* APR-encoded */
          apr_status_t apr_err;

          /* canonicalize case, and change all separators to '/'. */
          SVN_ERR (svn_path_cstring_from_utf8 (&apr_target, utf8_target,
                                               pool));
          apr_err = apr_filepath_merge (&truenamed_target, "", apr_target,
                                        APR_FILEPATH_TRUENAME, pool);

          if (!apr_err)
            /* We have a canonicalized APR-encoded target now. */
            apr_target = truenamed_target;
          else if (APR_STATUS_IS_ENOENT (apr_err))
            /* It's okay for the file to not exist, that just means we
               have to accept the case given to the client. We'll use
               the original APR-encoded target. */
            ;
          else
            return svn_error_createf (apr_err, NULL,
                                      "Error resolving case of '%s'.",
                                      utf8_target);

          /* convert back to UTF-8. */
          SVN_ERR (svn_path_cstring_to_utf8 (&target, apr_target, pool));
          target = svn_path_canonicalize (target, pool);

          /* If this target is a Subversion administrative directory,
             skip it.  TODO: Perhaps this check should not call the
             target a SVN admin dir unless svn_wc_check_wc passes on
             the target, too? */
          base_name = svn_path_basename (target, pool);
          if (! strcmp (base_name, SVN_WC_ADM_DIR_NAME))
            continue;
        }

      (*((const char **) apr_array_push (output_targets))) = target;
    }


  /* kff todo: need to remove redundancies from targets before
     passing it to the cmd_func. */

  if (extract_revisions)
    {
      svn_opt_revision_t *firstrev = NULL, *secondrev = NULL;

      for (i = 0; i < output_targets->nelts; i++)
        {
          const char *truepath;
          svn_opt_revision_t temprev; 
          const char *path = ((const char **) (output_targets->elts))[i];

          SVN_ERR (parse_path (&temprev, &truepath, path, pool));

          if (temprev.kind != svn_opt_revision_unspecified)
            {
              ((const char **) (output_targets->elts))[i] = 
                svn_path_canonicalize (truepath, pool);

              if (! firstrev)
                {
                  firstrev = apr_pcalloc (pool, sizeof (*firstrev));
                  firstrev->kind = temprev.kind;
                  firstrev->value = temprev.value;
                }
              else if (! secondrev)
                {
                  secondrev = apr_pcalloc (pool, sizeof (*secondrev));
                  secondrev->kind = temprev.kind;
                  secondrev->value = temprev.value;
                }
              else
                break;
            }
        }

      if (firstrev)
        {
          start_revision->kind = firstrev->kind;
          start_revision->value = firstrev->value;
        }
      
      if (secondrev)
        {
          end_revision->kind = secondrev->kind;
          end_revision->value = secondrev->value;
        }
    }
  
  *targets_p = output_targets;
  return SVN_NO_ERROR;
}


/* Print version info for PGM_NAME.  If QUIET is  true, print in
 * brief.  Else if QUIET is not true, print the version more
 * verbosely, and if FOOTER is non-null, print it following the
 * version information.
 *
 * Use POOL for temporary allocations.
 */
static svn_error_t *
print_version_info (const char *pgm_name,
                    const char *footer,
                    svn_boolean_t quiet,
                    apr_pool_t *pool)
{
  static const char info[] =
    "Copyright (C) 2000-2003 CollabNet.\n"
    "Subversion is open source software, see http://subversion.tigris.org/\n";

  if (quiet)
    {
      printf ("%s\n", SVN_VER_NUMBER);
      return SVN_NO_ERROR;
    }

  printf ("%s, version %s\n", pgm_name, SVN_VERSION);
  printf ("   compiled %s, %s\n\n", __DATE__, __TIME__);
  printf ("%s\n", info);

  if (footer)
    {
      const char *footer_native;

      /* ### Back when this code lived in help-cmd.c, prior to rev
       * 3250 or so, this was the only part of the version output that
       * did utf8-to-native conversion.  So I've kept that conversion,
       * but shouldn't the rest of the code do something similar?
       */
      SVN_ERR (svn_utf_cstring_from_utf8 (&footer_native, footer, pool));
      printf ("%s\n", footer_native);
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_opt_print_help (apr_getopt_t *os,
                    const char *pgm_name,
                    svn_boolean_t print_version,
                    svn_boolean_t quiet,
                    const char *version_footer,
                    const char *header,
                    const svn_opt_subcommand_desc_t *cmd_table,
                    const apr_getopt_option_t *option_table,
                    const char *footer,
                    apr_pool_t *pool)
{
  apr_array_header_t *targets = NULL;
  int i;

  if (os)
    SVN_ERR (svn_opt_parse_all_args (&targets, os, pool));

  if (targets && targets->nelts)  /* help on subcommand(s) requested */
    for (i = 0; i < targets->nelts; i++)
      {
        svn_opt_subcommand_help (((const char **) (targets->elts))[i],
                                 cmd_table, option_table, pool);
      }
  else if (print_version)   /* just --version */
    SVN_ERR (print_version_info (pgm_name, version_footer, quiet, pool));
  else if (os && !targets->nelts)            /* `-h', `--help', or `help' */
    svn_opt_print_generic_help (header,
                                cmd_table,
                                option_table,
                                footer,
                                pool,
                                stdout);
  else                                       /* unknown option or cmd */
    fprintf (stderr, "Type `%s help' for usage.\n", pgm_name);

  return SVN_NO_ERROR;
}
