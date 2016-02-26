/** \file complete.c Functions related to tab-completion.

  These functions are used for storing and retrieving tab-completion data, as well as for performing tab-completion.
*/
#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <stddef.h>
#include <wchar.h>
#include <wctype.h>
#include <pwd.h>
#include <pthread.h>
#include <algorithm>
#include <list>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "fallback.h" // IWYU pragma: keep
#include "util.h"

#include "wildcard.h"
#include "proc.h"
#include "parser.h"
#include "function.h"
#include "complete.h"
#include "builtin.h"
#include "env.h"
#include "exec.h"
#include "expand.h"
#include "common.h"
#include "parse_util.h"
#include "wutil.h"
#include "path.h"
#include "parse_tree.h"
#include "iothread.h"
#include "docopt_registration.h"
#include "autoload.h"
#include "parse_constants.h"
#include "docopt_fish.h"

typedef docopt_fish::argument_parser_t<wcstring> docopt_parser_t;

typedef docopt_fish::base_annotated_option_t<wcstring> legacy_option_t;

/*
  Completion description strings, mostly for different types of files, such as sockets, block devices, etc.

  There are a few more completion description strings defined in
  expand.c. Maybe all completion description strings should be defined
  in the same file?
*/

/**
   Description for ~USER completion
*/
#define COMPLETE_USER_DESC _( L"Home for %ls" )

/**
   Description for short variables. The value is concatenated to this description
*/
#define COMPLETE_VAR_DESC_VAL _( L"Variable: %ls" )

/**
   The special cased translation macro for completions. The empty
   string needs to be special cased, since it can occur, and should
   not be translated. (Gettext returns the version information as the
   response)
*/
#ifdef USE_GETTEXT
static const wchar_t *C_(const wcstring &s)
{
    return s.empty() ? L"" : wgettext(s.c_str());
}
#else
static const wcstring &C_(const wcstring &s)
{
    return s;
}
#endif


static void complete_load(const wcstring &name, bool reload);

/* Testing apparatus */
const wcstring_list_t *s_override_variable_names = NULL;

void complete_set_variable_names(const wcstring_list_t *names)
{
    s_override_variable_names = names;
}

static inline wcstring_list_t complete_get_variable_names(void)
{
    if (s_override_variable_names != NULL)
    {
        return *s_override_variable_names;
    }
    else
    {
        return env_get_names(0);
    }
}

/* Last value used in the order field of completion_entry_t */
static unsigned int kCompleteOrder = 0;

/**
   Struct describing a command completion
*/
typedef std::vector<legacy_option_t> legacy_option_list_t;
class completion_entry_t
{
public:
    /** List of all legacy options */
    legacy_option_list_t options;
    
    void invalidate_handle();
    shared_ptr<docopt_parser_t> ensure_handle();
    
    /** Handle on current docopt. Set to NULL if it must be recomputed. */
    shared_ptr<docopt_parser_t> doc_handle;
    
public:

    /** Command string */
    const wcstring cmd;

    /** True if command is a path */
    const bool cmd_is_path;

    /** True if no other options than the ones supplied are possible */
    bool authoritative;
    
    /** Order for when this completion was created. This aids in outputting completions sorted by time. */
    const unsigned int order;

    /** Adds or removes an option. */
    void add_option(const legacy_option_t &opt);
    bool remove_option(const wcstring &option, complete_option_type_t type);
    void remove_all_options();
    
    completion_entry_t(const wcstring &c, bool type, bool author) :
        doc_handle(),
        cmd(c),
        cmd_is_path(type),
        authoritative(author),
        order(++kCompleteOrder)
    {
    }
};

/** Set of all completion entries */
struct completion_entry_set_comparer
{
    /** Comparison for std::set */
    bool operator()(completion_entry_t *p1, completion_entry_t *p2) const
    {
        /* Paths always come last for no particular reason */
        if (p1->cmd_is_path != p2->cmd_is_path)
        {
            return p1->cmd_is_path < p2->cmd_is_path;
        }
        else
        {
            return p1->cmd < p2->cmd;
        }
    }
};
typedef std::set<completion_entry_t *, completion_entry_set_comparer> completion_entry_set_t;
static completion_entry_set_t completion_set;

// Comparison function to sort completions by their order field
static bool compare_completions_by_order(const completion_entry_t *p1, const completion_entry_t *p2)
{
    return p1->order < p2->order;
}

/** The lock that guards the list of completion entries */
static pthread_mutex_t completion_lock = PTHREAD_MUTEX_INITIALIZER;

/* Generates "unique" variable names for use in legacy completions */
static wcstring unique_variable_name()
{
    static uint64_t kNextIndex = 0;
    ASSERT_IS_LOCKED(completion_lock);
    return format_string(L"<%llu>", ++kNextIndex);
}

void completion_entry_t::add_option(const legacy_option_t &opt)
{
    ASSERT_IS_LOCKED(completion_lock);
    this->invalidate_handle();
    options.push_back(opt);
}

// Save a little binary size by preventing this from being inlined everywhere
completion_t::~completion_t()
{
}

/* Clear the COMPLETE_AUTO_SPACE flag, and set COMPLETE_NO_SPACE appropriately depending on the suffix of the string */
static complete_flags_t resolve_auto_space(const wcstring &comp, complete_flags_t flags)
{
    if (flags & COMPLETE_AUTO_SPACE)
    {
        flags = flags & ~COMPLETE_AUTO_SPACE;
        size_t len = comp.size();
        if (len > 0  && (wcschr(L"/=@:", comp.at(len-1)) != 0))
            flags |= COMPLETE_NO_SPACE;
    }
    return flags;
}

/* completion_t functions. Note that the constructor resolves flags! */
completion_t::completion_t(const wcstring &comp, const wcstring &desc, string_fuzzy_match_t mat, complete_flags_t flags_val) :
    completion(comp),
    description(desc),
    match(mat),
    flags(resolve_auto_space(comp, flags_val))
{
}

completion_t::completion_t(const completion_t &him) : completion(him.completion), description(him.description), match(him.match), flags(him.flags)
{
}

completion_t &completion_t::operator=(const completion_t &him)
{
    if (this != &him)
    {
        this->completion = him.completion;
        this->description = him.description;
        this->match = him.match;
        this->flags = him.flags;
    }
    return *this;
}

bool completion_t::is_naturally_less_than(const completion_t &a, const completion_t &b)
{
    return wcsfilecmp(a.completion.c_str(), b.completion.c_str()) < 0;
}

bool completion_t::is_alphabetically_equal_to(const completion_t &a, const completion_t &b)
{
    return a.completion == b.completion;
}

void completion_t::prepend_token_prefix(const wcstring &prefix)
{
    if (this->flags & COMPLETE_REPLACES_TOKEN)
    {
        this->completion.insert(0, prefix);
    }
}


static bool compare_completions_by_match_type(const completion_t &a, const completion_t &b)
{
    return a.match.type < b.match.type;
}

void completions_sort_and_prioritize(std::vector<completion_t> *comps)
{
    /* Find the best match type */
    fuzzy_match_type_t best_type = fuzzy_match_none;
    for (size_t i=0; i < comps->size(); i++)
    {
        best_type = std::min(best_type, comps->at(i).match.type);
    }
    /* If the best type is an exact match, reduce it to prefix match. Otherwise a tab completion will only show one match if it matches a file exactly. (see issue #959) */
    if (best_type == fuzzy_match_exact)
    {
        best_type = fuzzy_match_prefix;
    }
    
    /* Throw out completions whose match types are less suitable than the best. */
    size_t i = comps->size();
    while (i--)
    {
        if (comps->at(i).match.type > best_type)
        {
            comps->erase(comps->begin() + i);
        }
    }
    
    /* Remove duplicates */
    sort(comps->begin(), comps->end(), completion_t::is_naturally_less_than);
    comps->erase(std::unique(comps->begin(), comps->end(), completion_t::is_alphabetically_equal_to), comps->end());
    
    /* Sort the remainder by match type. They're already sorted alphabetically */
    stable_sort(comps->begin(), comps->end(), compare_completions_by_match_type);
}

/** Class representing an attempt to compute completions */
class completer_t
{
    const completion_request_flags_t flags;
    const wcstring initial_cmd;
    std::vector<completion_t> completions;
    const env_vars_snapshot_t &vars; //transient, stack-allocated

    /** Table of completions conditions that have already been tested and the corresponding test results */
    typedef std::map<wcstring, bool> condition_cache_t;
    condition_cache_t condition_cache;

    enum complete_type_t
    {
        COMPLETE_DEFAULT,
        COMPLETE_AUTOSUGGEST
    };

    complete_type_t type() const
    {
        return (flags & COMPLETION_REQUEST_AUTOSUGGESTION) ? COMPLETE_AUTOSUGGEST : COMPLETE_DEFAULT;
    }

    bool wants_descriptions() const
    {
        return !!(flags & COMPLETION_REQUEST_DESCRIPTIONS);
    }

    bool fuzzy() const
    {
        return !!(flags & COMPLETION_REQUEST_FUZZY_MATCH);
    }

    fuzzy_match_type_t max_fuzzy_match_type() const
    {
        /* If we are doing fuzzy matching, request all types; if not request only prefix matching */
        return (flags & COMPLETION_REQUEST_FUZZY_MATCH) ? fuzzy_match_none : fuzzy_match_prefix_case_insensitive;
    }


public:
    completer_t(const wcstring &c, completion_request_flags_t f, const env_vars_snapshot_t &evs) :
        flags(f),
        initial_cmd(c),
        vars(evs)
    {
    }

    bool empty() const
    {
        return completions.empty();
    }
    const std::vector<completion_t> &get_completions(void)
    {
        return completions;
    }

    bool try_complete_variable(const wcstring &str);
    bool try_complete_user(const wcstring &str);
    
    bool complete_from_docopt(const wcstring &cmd_orig, const parse_node_tree_t &tree, const parse_node_tree_t::parse_node_list_t &arg_nodes, const wcstring &src, bool cursor_in_last_arg);

    void complete_param_expand(const wcstring &str, bool do_file, bool handle_as_special_cd = false);
    
    void complete_special_cd(const wcstring &str);

    void complete_cmd(const wcstring &str,
                      bool use_function,
                      bool use_builtin,
                      bool use_command,
                      bool use_implicit_cd);

    void complete_from_args(const wcstring &str,
                            const wcstring &args,
                            const wcstring &desc,
                            complete_flags_t flags);

    void complete_cmd_desc(const wcstring &str);

    bool complete_variable(const wcstring &str, size_t start_offset);

    bool condition_test(const wcstring &condition);

    void complete_strings(const wcstring &wc_escaped,
                          const wchar_t *desc,
                          wcstring(*desc_func)(const wcstring &),
                          std::vector<completion_t> &possible_comp,
                          complete_flags_t flags);

    expand_flags_t expand_flags() const
    {
        /* Never do command substitution in autosuggestions. Sadly, we also can't yet do job expansion because it's not thread safe. */
        expand_flags_t result = 0;
        if (this->type() == COMPLETE_AUTOSUGGEST)
            result |= EXPAND_SKIP_CMDSUBST;

        /* Allow fuzzy matching */
        if (this->fuzzy())
            result |= EXPAND_FUZZY_MATCH;

        return result;
    }
};

/* Autoloader for completions */
class completion_autoload_t : public autoload_t
{
public:
    completion_autoload_t();
    virtual void command_removed(const wcstring &cmd);
};

static completion_autoload_t completion_autoloader;

/** Constructor */
completion_autoload_t::completion_autoload_t() : autoload_t(L"fish_complete_path", NULL, 0)
{
}

/** Callback when an autoloaded completion is removed */
void completion_autoload_t::command_removed(const wcstring &cmd)
{
    complete_remove_all(cmd, false /* not a path */);
}


/** Create a new completion entry. */
void append_completion(std::vector<completion_t> *completions, const wcstring &comp, const wcstring &desc, complete_flags_t flags, string_fuzzy_match_t match)
{
    /* If we just constructed the completion and used push_back, we would get two string copies. Try to avoid that by making a stubby completion in the vector first, and then copying our string in. Note that completion_t's constructor will munge 'flags' so it's important that we pass those to the constructor.

       Nasty hack for #1241 - since the constructor needs the completion string to resolve AUTO_SPACE, and we aren't providing it with the completion, we have to do the resolution ourselves. We should get this resolving out of the constructor.
    */
    assert(completions != NULL);
    const wcstring empty;
    completions->push_back(completion_t(empty, empty, match, resolve_auto_space(comp, flags)));
    completion_t *last = &completions->back();
    last->completion = comp;
    last->description = desc;
}

/**
   Test if the specified script returns zero. The result is cached, so
   that if multiple completions use the same condition, it needs only
   be evaluated once. condition_cache_clear must be called after a
   completion run to make sure that there are no stale completions.
*/
bool completer_t::condition_test(const wcstring &condition)
{
    if (condition.empty())
    {
        return true;
    }

    if (this->type() == COMPLETE_AUTOSUGGEST)
    {
        /* Autosuggestion can't support conditions */
        return false;
    }

    ASSERT_IS_MAIN_THREAD();

    bool test_res;
    condition_cache_t::iterator cached_entry = condition_cache.find(condition);
    if (cached_entry == condition_cache.end())
    {
        /* Compute new value and reinsert it */
        test_res = (0 == exec_subshell(condition, false /* don't apply exit status */));
        condition_cache[condition] = test_res;
    }
    else
    {
        /* Use the old value */
        test_res = cached_entry->second;
    }
    return test_res;
}


/** Search for an exactly matching completion entry. Must be called while locked. */
static completion_entry_t *complete_find_exact_entry(const wcstring &cmd, const bool cmd_is_path)
{
    ASSERT_IS_LOCKED(completion_lock);
    completion_entry_t *result = NULL;
    completion_entry_t tmp_entry(cmd, cmd_is_path, false);
    completion_entry_set_t::iterator iter = completion_set.find(&tmp_entry);
    if (iter != completion_set.end())
    {
        result = *iter;
    }
    return result;
}

/** Locate the specified entry. Create it if it doesn't exist. Must be called while locked. */
static completion_entry_t *complete_get_exact_entry(const wcstring &cmd, bool cmd_is_path)
{
    ASSERT_IS_LOCKED(completion_lock);
    completion_entry_t *c;

    c = complete_find_exact_entry(cmd, cmd_is_path);

    if (c == NULL)
    {
        c = new completion_entry_t(cmd, cmd_is_path, false);
        completion_set.insert(c);
    }

    return c;
}


void complete_set_authoritative(const wchar_t *cmd, bool cmd_is_path, bool authoritative)
{
    completion_entry_t *c;

    CHECK(cmd,);
    scoped_lock lock(completion_lock);
    c = complete_get_exact_entry(cmd, cmd_is_path);
    c->authoritative = authoritative;
}


void complete_add(const wchar_t *cmd,
                  bool cmd_is_path,
                  const wcstring &option,
                  complete_option_type_t option_type,
                  complete_argument_flags_t arg_flags,
                  const wchar_t *condition,
                  const wchar_t *comp,
                  const wchar_t *desc,
                  complete_flags_t flags)
{
    CHECK(cmd,);
    // option should be  empty iff the option type is arguments only
    assert(option.empty() == (option_type == option_type_args_only));

    /* Lock the lock that allows us to edit the completion entry list */
    scoped_lock lock(completion_lock);

    completion_entry_t *c;
    c = complete_get_exact_entry(cmd, cmd_is_path);

    /* Create our new option */
    legacy_option_t lopt;
    lopt.option = option;
    int expected_dash_count = 1;
    switch (option_type)
    {
        case option_type_args_only:
        case option_type_short:
            lopt.type = legacy_option_t::single_short;
            break;
        case option_type_single_long:
            lopt.type = legacy_option_t::single_long;
            break;
        case option_type_double_long:
            lopt.type = legacy_option_t::double_long;
            expected_dash_count = 2;
            break;
    }
    if (! option.empty())
    {
        lopt.option.assign(expected_dash_count, L'-');
        lopt.option.append(option);
    }
    if (comp != NULL)
    {
        lopt.value_name = unique_variable_name();
        lopt.metadata.command = comp;
    }
    if (condition != NULL)
    {
        lopt.metadata.condition = condition;
    }
    if (desc != NULL)
    {
        lopt.metadata.description = desc;
    }
    lopt.metadata.tag = arg_flags;
    
    c->invalidate_handle();
    c->options.push_back(lopt);
}

// Have to determine ourselves if this is a path
static void parse_cmd_string(const wcstring &str, wcstring &path, wcstring &cmd);
static shared_ptr<docopt_parser_t> complete_rebuild_docopt_as_necessary(const wcstring &cmd_or_path)
{
    shared_ptr<docopt_parser_t> result;
    wcstring cmd, path;
    parse_cmd_string(cmd_or_path, path, cmd);

    scoped_lock lock(completion_lock);
    if (! cmd.empty())
    {
        completion_entry_t *c = complete_find_exact_entry(cmd, false);
        if (c != NULL)
        {
            result = c->ensure_handle();
        }
    }
    if (result.get() == NULL && ! path.empty())
    {
        completion_entry_t *c = complete_find_exact_entry(path, true);
        if (c != NULL)
        {
            result = c->ensure_handle();
        }
    }
    return result;
}

void completion_entry_t::invalidate_handle()
{
    ASSERT_IS_LOCKED(completion_lock);
    this->doc_handle.reset();
}

shared_ptr<docopt_parser_t> completion_entry_t::ensure_handle()
{
    ASSERT_IS_LOCKED(completion_lock);
    if (this->doc_handle.get() == NULL && ! this->options.empty())
    {
        this->doc_handle.reset(new docopt_parser_t());
        this->doc_handle->set_options(this->options);
    }
    return this->doc_handle;
}

/**
   Remove all completion options in the specified entry that match the
   specified short / long option strings. Returns true if it is now
   empty and should be deleted, false if it's not empty. Must be called while locked.
*/
bool completion_entry_t::remove_option(const wcstring &option, complete_option_type_t type)
{
    ASSERT_IS_LOCKED(completion_lock);
    legacy_option_list_t::iterator iter = this->options.begin();
    while (iter != this->options.end())
    {
        if (iter->option == option && iter->type == type)
        {
            this->invalidate_handle();
            iter = this->options.erase(iter);
        }
        else
        {
            /* Just go to the next one */
            ++iter;
        }
    }
    return this->options.empty();
}

void completion_entry_t::remove_all_options()
{
    ASSERT_IS_LOCKED(completion_lock);
    this->invalidate_handle();
    this->options.clear();
}


void complete_remove(const wcstring &cmd, bool cmd_is_path, const wcstring &option, complete_option_type_t type)
{
    scoped_lock lock(completion_lock);

    completion_entry_t tmp_entry(cmd, cmd_is_path, false);
    completion_entry_set_t::iterator iter = completion_set.find(&tmp_entry);
    if (iter != completion_set.end())
    {
        completion_entry_t *entry = *iter;
        bool delete_it = entry->remove_option(option, type);
        if (delete_it)
        {
            /* Delete this entry */
            completion_set.erase(iter);
            delete entry;
        }
    }
}

void complete_remove_all(const wcstring &cmd, bool cmd_is_path)
{
    scoped_lock lock(completion_lock);
    
    completion_entry_t tmp_entry(cmd, cmd_is_path, false);
    completion_entry_set_t::iterator iter = completion_set.find(&tmp_entry);
    if (iter != completion_set.end())
    {
        completion_entry_t *entry = *iter;
        entry->remove_all_options();
        completion_set.erase(iter);
        delete entry;
    }
}


/**
   Find the full path and commandname from a command string 'str'.
*/
static void parse_cmd_string(const wcstring &str, wcstring &path, wcstring &cmd)
{
    if (! path_get_path(str, &path))
    {
        /** Use the empty string as the 'path' for commands that can not be found. */
        path = L"";
    }

    /* Make sure the path is not included in the command */
    size_t last_slash = str.find_last_of(L'/');
    if (last_slash != wcstring::npos)
    {
        cmd = str.substr(last_slash + 1);
    }
    else
    {
        cmd = str;
    }
}

/**
   Copy any strings in possible_comp which have the specified prefix
   to the completer's completion array. The prefix may contain wildcards.
   The output will consist of completion_t structs.

   There are three ways to specify descriptions for each
   completion. Firstly, if a description has already been added to the
   completion, it is _not_ replaced. Secondly, if the desc_func
   function is specified, use it to determine a dynamic
   completion. Thirdly, if none of the above are available, the desc
   string is used as a description.

   \param wc_escaped the prefix, possibly containing wildcards. The wildcard should not have been unescaped, i.e. '*' should be used for any string, not the ANY_STRING character.
   \param desc the default description, used for completions with no embedded description. The description _may_ contain a COMPLETE_SEP character, if not, one will be prefixed to it
   \param desc_func the function that generates a description for those completions witout an embedded description
   \param possible_comp the list of possible completions to iterate over
*/

void completer_t::complete_strings(const wcstring &wc_escaped,
                                   const wchar_t *desc,
                                   wcstring(*desc_func)(const wcstring &),
                                   std::vector<completion_t> &possible_comp,
                                   complete_flags_t flags)
{
    wcstring tmp = wc_escaped;
    if (! expand_one(tmp, EXPAND_SKIP_CMDSUBST | EXPAND_SKIP_WILDCARDS | this->expand_flags(), NULL))
        return;

    const wcstring wc = parse_util_unescape_wildcards(tmp);

    for (size_t i=0; i< possible_comp.size(); i++)
    {
        wcstring temp = possible_comp.at(i).completion;
        const wchar_t *next_str = temp.empty()?NULL:temp.c_str();

        if (next_str)
        {
            wildcard_complete(next_str, wc.c_str(), desc, desc_func, &this->completions, this->expand_flags(), flags);
        }
    }
}

/**
   If command to complete is short enough, substitute
   the description with the whatis information for the executable.
*/
void completer_t::complete_cmd_desc(const wcstring &str)
{
    ASSERT_IS_MAIN_THREAD();

    const wchar_t *cmd_start;
    int skip;

    const wchar_t * const cmd = str.c_str();
    cmd_start=wcsrchr(cmd, L'/');

    if (cmd_start)
        cmd_start++;
    else
        cmd_start = cmd;

    /*
      Using apropos with a single-character search term produces far
      to many results - require at least two characters if we don't
      know the location of the whatis-database.
    */
    if (wcslen(cmd_start) < 2)
        return;

    if (wildcard_has(cmd_start, 0))
    {
        return;
    }

    skip = 1;

    for (size_t i=0; i< this->completions.size(); i++)
    {
        const completion_t &c = this->completions.at(i);

        if (c.completion.empty() || (c.completion[c.completion.size()-1] != L'/'))
        {
            skip = 0;
            break;
        }

    }

    if (skip)
    {
        return;
    }


    wcstring lookup_cmd(L"__fish_describe_command ");
    lookup_cmd.append(escape_string(cmd_start, 1));

    std::map<wcstring, wcstring> lookup;

    /*
      First locate a list of possible descriptions using a single
      call to apropos or a direct search if we know the location
      of the whatis database. This can take some time on slower
      systems with a large set of manuals, but it should be ok
      since apropos is only called once.
    */
    wcstring_list_t list;
    if (exec_subshell(lookup_cmd, list, false /* don't apply exit status */) != -1)
    {

        /*
          Then discard anything that is not a possible completion and put
          the result into a hashtable with the completion as key and the
          description as value.

          Should be reasonably fast, since no memory allocations are needed.
        */
        for (size_t i=0; i < list.size(); i++)
        {
            const wcstring &elstr = list.at(i);

            const wcstring fullkey(elstr, wcslen(cmd_start));

            size_t tab_idx = fullkey.find(L'\t');
            if (tab_idx == wcstring::npos)
                continue;

            const wcstring key(fullkey, 0, tab_idx);
            wcstring val(fullkey, tab_idx + 1);

            /*
              And once again I make sure the first character is uppercased
              because I like it that way, and I get to decide these
              things.
            */
            if (! val.empty())
                val[0]=towupper(val[0]);

            lookup[key] = val;
        }

        /*
          Then do a lookup on every completion and if a match is found,
          change to the new description.

          This needs to do a reallocation for every description added, but
          there shouldn't be that many completions, so it should be ok.
        */
        for (size_t i=0; i<this->completions.size(); i++)
        {
            completion_t &completion = this->completions.at(i);
            const wcstring &el = completion.completion;
            if (el.empty())
                continue;

            std::map<wcstring, wcstring>::iterator new_desc_iter = lookup.find(el);
            if (new_desc_iter != lookup.end())
                completion.description = new_desc_iter->second;
        }
    }

}

/**
   Returns a description for the specified function, or an empty string if none
*/
static wcstring complete_function_desc(const wcstring &fn)
{
    wcstring result;
    bool has_description = function_get_desc(fn, &result);
    if (! has_description)
    {
        function_get_definition(fn, &result);
    }
    return result;
}


/**
   Complete the specified command name. Search for executables in the
   path, executables defined using an absolute path, functions,
   builtins and directories for implicit cd commands.

   \param cmd the command string to find completions for

   \param comp the list to add all completions to
*/
void completer_t::complete_cmd(const wcstring &str_cmd, bool use_function, bool use_builtin, bool use_command, bool use_implicit_cd)
{
    /* Paranoia */
    if (str_cmd.empty())
        return;

    std::vector<completion_t> possible_comp;

    env_var_t cdpath = this->vars.get(L"CDPATH");
    if (cdpath.missing_or_empty())
        cdpath = L".";

    if (use_command)
    {

        if (expand_string(str_cmd, &this->completions, EXPAND_FOR_COMPLETIONS | EXECUTABLES_ONLY | this->expand_flags(), NULL) != EXPAND_ERROR)
        {
            if (this->wants_descriptions())
            {
                this->complete_cmd_desc(str_cmd);
            }
        }
    }
    if (use_implicit_cd)
    {
        if (!expand_string(str_cmd, &this->completions, EXPAND_FOR_COMPLETIONS | DIRECTORIES_ONLY | this->expand_flags(), NULL))
        {
            // Not valid as implicit cd.
        }
    }
    if (str_cmd.find(L'/') == wcstring::npos && str_cmd.at(0) != L'~')
    {
        if (use_command)
        {

            const env_var_t path = this->vars.get(L"PATH");
            if (!path.missing())
            {
                wcstring base_path;
                wcstokenizer tokenizer(path, ARRAY_SEP_STR);
                while (tokenizer.next(base_path))
                {
                    if (base_path.empty())
                        continue;

                    /* Make sure the base path ends with a slash */
                    if (base_path.at(base_path.size() - 1) != L'/')
                        base_path.push_back(L'/');

                    wcstring nxt_completion = base_path;
                    nxt_completion.append(str_cmd);

                    size_t prev_count =  this->completions.size();
                    expand_flags_t expand_flags = EXPAND_FOR_COMPLETIONS | EXECUTABLES_ONLY | EXPAND_NO_FUZZY_DIRECTORIES | this->expand_flags();
                    if (expand_string(nxt_completion,
                                      &this->completions,
                                      expand_flags, NULL) != EXPAND_ERROR)
                    {
                        /* For all new completions, if COMPLETE_REPLACES_TOKEN is set, then use only the last path component */
                        for (size_t i=prev_count; i< this->completions.size(); i++)
                        {
                            completion_t &c =  this->completions.at(i);
                            if (c.flags & COMPLETE_REPLACES_TOKEN)
                            {

                                c.completion.erase(0, base_path.size());
                            }
                        }
                    }
                }
                if (this->wants_descriptions())
                    this->complete_cmd_desc(str_cmd);
            }
        }

        if (use_function)
        {
            //function_get_names( &possible_comp, cmd[0] == L'_' );
            wcstring_list_t names = function_get_names(str_cmd.at(0) == L'_');
            for (size_t i=0; i < names.size(); i++)
            {
                append_completion(&possible_comp, names.at(i));
            }

            this->complete_strings(str_cmd, 0, &complete_function_desc, possible_comp, 0);
        }

        possible_comp.clear();

        if (use_builtin)
        {
            builtin_get_names(&possible_comp);
            this->complete_strings(str_cmd, 0, &builtin_get_desc, possible_comp, 0);
        }

    }
}


/**
   Evaluate the argument list (as supplied by complete -a) and insert
   any return matching completions. Matching is done using \c
   copy_strings_with_prefix, meaning the completion may contain
   wildcards. Logically, this is not always the right thing to do, but
   I have yet to come up with a case where this matters.

   \param str The string to complete.
   \param args The list of option arguments to be evaluated.
   \param desc Description of the completion
   \param comp_out The list into which the results will be inserted
*/
void completer_t::complete_from_args(const wcstring &str,
                                     const wcstring &args,
                                     const wcstring &desc,
                                     complete_flags_t flags)
{
    bool is_autosuggest = (this->type() == COMPLETE_AUTOSUGGEST);
    parser_t parser(is_autosuggest ? PARSER_TYPE_COMPLETIONS_ONLY : PARSER_TYPE_GENERAL, false /* don't show errors */);

    /* If type is COMPLETE_AUTOSUGGEST, it means we're on a background thread, so don't call proc_push_interactive */
    if (! is_autosuggest)
        proc_push_interactive(0);

    std::vector<completion_t> possible_comp;
    parser.expand_argument_list(args, &possible_comp);

    if (! is_autosuggest)
        proc_pop_interactive();

    this->complete_strings(escape_string(str, ESCAPE_ALL), desc.c_str(), 0, possible_comp, flags);
}

/* Load command-specific completions for the specified command. */
static void complete_load(const wcstring &name, bool reload)
{
    // we have to load this as a function, since it may define a --wraps or signature
    // see #2466
    function_load(name);
    completion_autoloader.load(name, reload);
}

// Performed on main thread, from background thread. Return type is ignored.
static int complete_load_no_reload(wcstring *name)
{
    assert(name != NULL);
    ASSERT_IS_MAIN_THREAD();
    complete_load(*name, false);
    return 0;
}

// Attempts to fetch completions from docopt. Returns false if file completion should be performed, true if they should be skipped.
bool completer_t::complete_from_docopt(const wcstring &cmd_unescape, const parse_node_tree_t &tree, const parse_node_tree_t::parse_node_list_t &arg_nodes, const wcstring &src, bool cursor_in_last_arg)
{
    bool suppress_file_completion = false;
    const complete_flags_t local_flags = COMPLETE_AUTO_SPACE;
    wcstring_list_t argv;
    argv.push_back(cmd_unescape);
    for (size_t i=0; i < arg_nodes.size(); i++)
    {
        bool allow_incomplete = cursor_in_last_arg && (i + 1 == arg_nodes.size());
        wcstring arg = arg_nodes.at(i)->get_source(src);
        // ignore args that fail to unescape
        if (unescape_string_in_place(&arg, allow_incomplete ? UNESCAPE_INCOMPLETE : UNESCAPE_DEFAULT))
        {
            argv.push_back(arg);
        }
    }
    
    wcstring last_arg;
    if (cursor_in_last_arg && ! argv.empty())
    {
        last_arg = argv.back();
        argv.pop_back();
    }
    
    // Only allow options as completions if the argument we're completing starts with a -
    const bool allow_options = string_prefixes_string(L"-", last_arg);
    
    // Get existing registrations, and maybe add our legacy parser
    docopt_registration_set_t regs = docopt_get_registrations(cmd_unescape);
    shared_ptr<docopt_parser_t> legacy_parser = complete_rebuild_docopt_as_necessary(cmd_unescape);
    if (legacy_parser.get() != NULL)
    {
        regs.add_legacy_parser(legacy_parser);
    }
    
    const std::vector<docopt_suggestion_t> suggestions = regs.suggest_next_argument(argv, flag_match_allow_incomplete);
    for (size_t i=0; i < suggestions.size(); i++)
    {
        const docopt_suggestion_t &suggestion = suggestions.at(i);
        
        if (!allow_options && string_prefixes_string(L"-", suggestion.token))
        {
            // Not allowing options
            continue;
        }
        
        // We need to test the condition in every branch, but we can do it after a fuzzy match in the options case
        if (string_prefixes_string(L"<", suggestion.token))
        {
            // Variable. Handle any commands.
            if (! suggestion.command.empty())
            {
                // Test the condition
                if (! condition_test(suggestion.condition)) {
                    continue;
                }

                this->complete_from_args(last_arg, suggestion.command, suggestion.description, local_flags);

                // Maybe suppress file completions
                suppress_file_completion = ! (suggestion.tag & argument_allow_files);
            }
        }
        else
        {
            if (last_arg.empty())
            {
                // Test the condition
                if (! condition_test(suggestion.condition)) {
                    continue;
                }

                // No partial argument to complete, just dump it in
                append_completion(&this->completions, suggestion.token, suggestion.description, local_flags);
                suppress_file_completion = ! (suggestion.tag & argument_allow_files);
            }
            else
            {
                // We have a partial argument, we have to match it against our last argument
                string_fuzzy_match_t match = string_fuzzy_match_string(last_arg, suggestion.token, this->max_fuzzy_match_type());
                if (match.type != fuzzy_match_none)
                {
                    // Test the condition
                    if (! condition_test(suggestion.condition)) {
                        continue;
                    }

                    if (match_type_requires_full_replacement(match.type))
                    {
                        append_completion(&this->completions, suggestion.token, suggestion.description, local_flags | COMPLETE_REPLACES_TOKEN, match);
                    }
                    else
                    {
                        // Append a prefix completion that starts after the last argument
                        append_completion(&this->completions, wcstring(suggestion.token, last_arg.size()), suggestion.description, local_flags, match);
                    }
                    suppress_file_completion = ! (suggestion.tag & argument_allow_files);
                }
            }
        }
    }
    return suppress_file_completion;
}

/**
   Perform generic (not command-specific) expansions on the specified string
*/
void completer_t::complete_param_expand(const wcstring &str, bool do_file, bool handle_as_special_cd)
{
    expand_flags_t flags = EXPAND_SKIP_CMDSUBST | EXPAND_FOR_COMPLETIONS | this->expand_flags();

    if (! do_file)
        flags |= EXPAND_SKIP_WILDCARDS;
    
    if (handle_as_special_cd && do_file)
    {
        flags |= DIRECTORIES_ONLY | EXPAND_SPECIAL_CD | EXPAND_NO_DESCRIPTIONS;
    }

    /* Squelch file descriptions per issue 254 */
    if (this->type() == COMPLETE_AUTOSUGGEST || do_file)
        flags |= EXPAND_NO_DESCRIPTIONS;

    /* We have the following cases:
     
     --foo=bar => expand just bar
     -foo=bar => expand just bar
     foo=bar => expand the whole thing, and also just bar
     
     We also support colon separator (#2178). If there's more than one, prefer the last one.
     */
    
    size_t sep_index = str.find_last_of(L"=:");
    bool complete_from_separator = (sep_index != wcstring::npos);
    bool complete_from_start = !complete_from_separator || !string_prefixes_string(L"-", str);
    
    if (complete_from_separator)
    {
        const wcstring sep_string = wcstring(str, sep_index + 1);
        std::vector<completion_t> local_completions;
        if (expand_string(sep_string,
                          &local_completions,
                          flags,
                          NULL) == EXPAND_ERROR)
        {
            debug(3, L"Error while expanding string '%ls'", sep_string.c_str());
        }
        
        /* Any COMPLETE_REPLACES_TOKEN will also stomp the separator. We need to "repair" them by inserting our separator and prefix. */
        const wcstring prefix_with_sep = wcstring(str, 0, sep_index + 1);
        for (size_t i=0; i < local_completions.size(); i++)
        {
            local_completions.at(i).prepend_token_prefix(prefix_with_sep);
        }
        this->completions.insert(this->completions.end(),
                                 local_completions.begin(),
                                 local_completions.end());
    }
    
    if (complete_from_start)
    {
        /* Don't do fuzzy matching for files if the string begins with a dash (#568). We could consider relaxing this if there was a preceding double-dash argument */
        if (string_prefixes_string(L"-", str))
            flags &= ~EXPAND_FUZZY_MATCH;
        
        if (expand_string(str,
                          &this->completions,
                          flags, NULL) == EXPAND_ERROR)
        {
            debug(3, L"Error while expanding string '%ls'", str.c_str());
        }
    }
}

/**
   Complete the specified string as an environment variable
*/
bool completer_t::complete_variable(const wcstring &str, size_t start_offset)
{
    const wchar_t * const whole_var = str.c_str();
    const wchar_t *var = &whole_var[start_offset];
    size_t varlen = wcslen(var);
    bool res = false;

    const wcstring_list_t names = complete_get_variable_names();
    for (size_t i=0; i<names.size(); i++)
    {
        const wcstring & env_name = names.at(i);

        string_fuzzy_match_t match = string_fuzzy_match_string(var, env_name, this->max_fuzzy_match_type());
        if (match.type == fuzzy_match_none)
        {
            // No match
            continue;
        }

        wcstring comp;
        int flags = 0;

        if (! match_type_requires_full_replacement(match.type))
        {
            // Take only the suffix
            comp.append(env_name.c_str() + varlen);
        }
        else
        {
            comp.append(whole_var, start_offset);
            comp.append(env_name);
            flags = COMPLETE_REPLACES_TOKEN | COMPLETE_DONT_ESCAPE;
        }

        wcstring desc;
        if (this->wants_descriptions())
        {
            // Can't use this->vars here, it could be any variable
            env_var_t value_unescaped = env_get_string(env_name);
            if (value_unescaped.missing())
                continue;

            wcstring value = expand_escape_variable(value_unescaped);
            if (this->type() != COMPLETE_AUTOSUGGEST)
                desc = format_string(COMPLETE_VAR_DESC_VAL, value.c_str());
        }

        append_completion(&this->completions,  comp, desc, flags, match);

        res = true;
    }

    return res;
}

bool completer_t::try_complete_variable(const wcstring &str)
{
    enum {e_unquoted, e_single_quoted, e_double_quoted} mode = e_unquoted;
    const size_t len = str.size();

    /* Get the position of the dollar heading a run of valid variable characters. -1 means none. */
    size_t variable_start = -1;

    for (size_t in_pos=0; in_pos<len; in_pos++)
    {
        wchar_t c = str.at(in_pos);
        if (! wcsvarchr(c))
        {
            /* This character cannot be in a variable, reset the dollar */
            variable_start = -1;
        }

        switch (c)
        {
            case L'\\':
                in_pos++;
                break;

            case L'$':
                if (mode == e_unquoted || mode == e_double_quoted)
                {
                    variable_start = in_pos;
                }
                break;

            case L'\'':
                if (mode == e_single_quoted)
                {
                    mode = e_unquoted;
                }
                else if (mode == e_unquoted)
                {
                    mode = e_single_quoted;
                }
                break;

            case L'"':
                if (mode == e_double_quoted)
                {
                    mode = e_unquoted;
                }
                else if (mode == e_unquoted)
                {
                    mode = e_double_quoted;
                }
                break;
        }
    }

    /* Now complete if we have a variable start that's also not the last character */
    bool result = false;
    if (variable_start != static_cast<size_t>(-1) && variable_start + 1 < len)
    {
        result = this->complete_variable(str, variable_start + 1);
    }
    return result;
}

/**
   Try to complete the specified string as a username. This is used by
   ~USER type expansion.

   \return 0 if unable to complete, 1 otherwise
*/
bool completer_t::try_complete_user(const wcstring &str)
{
    const wchar_t *cmd = str.c_str();
    const wchar_t *first_char=cmd;
    int res=0;
    double start_time = timef();

    if (*first_char ==L'~' && !wcschr(first_char, L'/'))
    {
        const wchar_t *user_name = first_char+1;
        const wchar_t *name_end = wcschr(user_name, L'~');
        if (name_end == 0)
        {
            struct passwd *pw;
            size_t name_len = wcslen(user_name);

            setpwent();

            while ((pw=getpwent()) != 0)
            {
                double current_time = timef();

                if (current_time - start_time > 0.2)
                {
                    return 1;
                }

                if (pw->pw_name)
                {
                    const wcstring pw_name_str = str2wcstring(pw->pw_name);
                    const wchar_t *pw_name = pw_name_str.c_str();
                    if (wcsncmp(user_name, pw_name, name_len)==0)
                    {
                        wcstring desc = format_string(COMPLETE_USER_DESC, pw_name);
                        append_completion(&this->completions,
                                          &pw_name[name_len],
                                          desc,
                                          COMPLETE_NO_SPACE);

                        res=1;
                    }
                    else if (wcsncasecmp(user_name, pw_name, name_len)==0)
                    {
                        wcstring name = format_string(L"~%ls", pw_name);
                        wcstring desc = format_string(COMPLETE_USER_DESC, pw_name);

                        append_completion(&this->completions,
                                          name,
                                          desc,
                                          COMPLETE_REPLACES_TOKEN | COMPLETE_DONT_ESCAPE | COMPLETE_NO_SPACE);
                        res=1;
                    }
                }
            }
            endpwent();
        }
    }

    return res;
}

void complete(const wcstring &cmd_with_subcmds, std::vector<completion_t> *out_comps, completion_request_flags_t flags, const env_vars_snapshot_t &vars)
{
    /* Determine the innermost subcommand */
    const wchar_t *cmdsubst_begin, *cmdsubst_end;
    parse_util_cmdsubst_extent(cmd_with_subcmds.c_str(), cmd_with_subcmds.size(), &cmdsubst_begin, &cmdsubst_end);
    assert(cmdsubst_begin != NULL && cmdsubst_end != NULL && cmdsubst_end >= cmdsubst_begin);
    const wcstring cmd = wcstring(cmdsubst_begin, cmdsubst_end - cmdsubst_begin);

    /* Make our completer */
    completer_t completer(cmd, flags, vars);

    wcstring current_command;
    const size_t pos = cmd.size();
    bool done=false;
    bool use_command = 1;
    bool use_function = 1;
    bool use_builtin = 1;
    bool use_implicit_cd = 1;

    //debug( 1, L"Complete '%ls'", cmd.c_str() );

    const wchar_t *cmd_cstr = cmd.c_str();
    const wchar_t *tok_begin = NULL, *prev_begin = NULL, *prev_end = NULL;
    parse_util_token_extent(cmd_cstr, cmd.size(), &tok_begin, NULL, &prev_begin, &prev_end);

    /**
     If we are completing a variable name or a tilde expansion user
     name, we do that and return. No need for any other completions.
     */
    const wcstring current_token = tok_begin;

    /* Unconditionally complete variables and processes. This is a little weird since we will happily complete variables even in e.g. command position, despite the fact that they are invalid there. */
    if (!done)
    {
        done = completer.try_complete_variable(current_token) || completer.try_complete_user(current_token);
    }

    if (!done)
    {
        parse_node_tree_t tree;
        parse_tree_from_string(cmd, parse_flag_continue_after_error | parse_flag_accept_incomplete_tokens | parse_flag_include_comments, &tree, NULL);

        /* Find any plain statement that contains the position. We have to backtrack past spaces (#1261). So this will be at either the last space character, or after the end of the string */
        size_t adjusted_pos = pos;
        while (adjusted_pos > 0 && cmd.at(adjusted_pos - 1) == L' ')
        {
            adjusted_pos--;
        }

        const parse_node_t *plain_statement = tree.find_node_matching_source_location(symbol_plain_statement, adjusted_pos, NULL);
        if (plain_statement == NULL)
        {
            /* Not part of a plain statement. This could be e.g. a for loop header, case expression, etc. Do generic file completions (#1309). If we had to backtrack, it means there was whitespace; don't do an autosuggestion in that case. Also don't do it if we are just after a pipe, semicolon, or & (#1631), or in a comment.
            
            Overall this logic is a total mess. A better approach would be to return the "possible next token" from the parse tree directly (this data is available as the first of the sequence of nodes without source locations at the very end of the parse tree). */
            bool do_file = true;
            if (flags & COMPLETION_REQUEST_AUTOSUGGESTION)
            {
                if (adjusted_pos < pos)
                {
                    do_file = false;
                }
                else if (pos > 0)
                {
                    // If the previous character is in one of these types, we don't do file suggestions
                    parse_token_type_t bad_types[] = {parse_token_type_pipe, parse_token_type_end, parse_token_type_background, parse_special_type_comment};
                    for (size_t i=0; i < sizeof bad_types / sizeof *bad_types; i++)
                    {
                        if (tree.find_node_matching_source_location(bad_types[i], pos - 1, NULL))
                        {
                            do_file = false;
                            break;
                        }
                    }
                }
            }
            completer.complete_param_expand(current_token, do_file);
        }
        else
        {
            assert(plain_statement->has_source() && plain_statement->type == symbol_plain_statement);

            /* Get the command node */
            const parse_node_t *cmd_node = tree.get_child(*plain_statement, 0, parse_token_type_string);

            /* Get the actual command string */
            if (cmd_node != NULL)
                current_command = cmd_node->get_source(cmd);

            /* Check the decoration */
            switch (tree.decoration_for_plain_statement(*plain_statement))
            {
                case parse_statement_decoration_none:
                    use_command = true;
                    use_function = true;
                    use_builtin = true;
                    use_implicit_cd = true;
                    break;

                case parse_statement_decoration_command:
                case parse_statement_decoration_exec:
                    use_command = true;
                    use_function = false;
                    use_builtin = false;
                    use_implicit_cd = false;
                    break;

                case parse_statement_decoration_builtin:
                    use_command = false;
                    use_function = false;
                    use_builtin = true;
                    use_implicit_cd = false;
                    break;
            }

            if (cmd_node && cmd_node->location_in_or_at_end_of_source_range(pos))
            {
                /* Complete command filename */
                completer.complete_cmd(current_token, use_function, use_builtin, use_command, use_implicit_cd);
            }
            else
            {
                /* Get all the arguments */
                const parse_node_tree_t::parse_node_list_t all_arguments = tree.find_nodes(*plain_statement, symbol_argument);

                /* See whether we are in an argument. We may also be in a redirection, or nothing at all. */
                size_t matching_arg_index = -1;
                for (size_t i=0; i < all_arguments.size(); i++)
                {
                    const parse_node_t *node = all_arguments.at(i);
                    if (node->location_in_or_at_end_of_source_range(adjusted_pos))
                    {
                        matching_arg_index = i;
                        break;
                    }
                }

                bool had_ddash = false;
                wcstring current_argument, previous_argument;
                if (matching_arg_index != (size_t)(-1))
                {

                    const wcstring matching_arg = all_arguments.at(matching_arg_index)->get_source(cmd);
                    
                    /* If the cursor is in whitespace, then the "current" argument is empty and the previous argument is the matching one. But if the cursor was in or at the end of the argument, then the current argument is the matching one, and the previous argument is the one before it. */
                    bool cursor_in_whitespace = (adjusted_pos < pos);
                    if (cursor_in_whitespace)
                    {
                        current_argument = L"";
                        previous_argument = matching_arg;
                    }
                    else
                    {
                        current_argument = matching_arg;
                        if (matching_arg_index > 0)
                        {
                            previous_argument = all_arguments.at(matching_arg_index - 1)->get_source(cmd);
                        }
                    }

                    /* Check to see if we have a preceding double-dash */
                    for (size_t i=0; i < matching_arg_index; i++)
                    {
                        if (all_arguments.at(i)->get_source(cmd) == L"--")
                        {
                            had_ddash = true;
                            break;
                        }
                    }
                }
                
                /* If we are not in an argument, we may be in a redirection */
                bool in_redirection = false;
                if (matching_arg_index == (size_t)(-1))
                {
                    const parse_node_t *redirection = tree.find_node_matching_source_location(symbol_redirection, adjusted_pos, plain_statement);
                    in_redirection = (redirection != NULL);
                }
                
                bool do_file = false, handle_as_special_cd = false;
                if (in_redirection)
                {
                    do_file = true;
                }
                else
                {
                    wcstring original_command_unescape;
                    if (unescape_string(current_command, &original_command_unescape, UNESCAPE_DEFAULT))
                    {
                        // Have to walk over the command and its entire wrap chain
                        // If any command disables do_file, then they all do
                        do_file = true;
                        const wcstring_list_t wrap_chain = complete_get_wrap_chain(original_command_unescape);
                        for (size_t i=0; i < wrap_chain.size(); i++)
                        {
                            const wcstring &completing_command = wrap_chain.at(i);
                            
                            // Maybe load this completion
                            if (! (flags & COMPLETION_REQUEST_AUTOSUGGESTION))
                            {
                                ASSERT_IS_MAIN_THREAD();
                                complete_load(completing_command, true);
                            }
                            else
                            {
                                /* Maybe load this command (on the main thread) */
                                if (! completion_autoloader.has_tried_loading(cmd))
                                {
                                    iothread_perform_on_main(complete_load_no_reload, &completing_command);
                                }
                            }
                            
                            // Hackish, this. The first command in the chain is always the given command. For every command past the first, we need to create a transient commandline for builtin_commandline. But not for COMPLETION_REQUEST_AUTOSUGGESTION, which may occur on background threads.
                            builtin_commandline_scoped_transient_t *transient_cmd = NULL;
                            if (i == 0)
                            {
                                assert(wrap_chain.at(i) == completing_command);
                            }
                            else if (! (flags & COMPLETION_REQUEST_AUTOSUGGESTION))
                            {
                                assert(cmd_node != NULL);
                                wcstring faux_cmdline = cmd;
                                faux_cmdline.replace(cmd_node->source_start, cmd_node->source_length, wrap_chain.at(i));
                                transient_cmd = new builtin_commandline_scoped_transient_t(faux_cmdline);
                            }
                            
                            complete_rebuild_docopt_as_necessary(completing_command);
                            
                            // Perform docopt completions
                            bool cursor_in_last_arg = (adjusted_pos == pos);
                            if (completer.complete_from_docopt(completing_command, tree, all_arguments, cmd, cursor_in_last_arg))
                            {
                                do_file = false;
                            }
                            
                            delete transient_cmd; //may be null
                        }
                    }

                    /* If we have found no command specific completions at all, fall back to using file completions. */
                    if (completer.empty())
                        do_file = true;
                    
                    /* Hack. If we're cd, handle it specially (#1059, others) */
                    handle_as_special_cd = (original_command_unescape == L"cd");
                    
                    /* And if we're autosuggesting, and the token is empty, don't do file suggestions */
                    if ((flags & COMPLETION_REQUEST_AUTOSUGGESTION) && current_argument.empty())
                    {
                        do_file = false;
                    }
                }

                /* This function wants the unescaped string */
                completer.complete_param_expand(current_token, do_file, handle_as_special_cd);
            }
        }
    }

    *out_comps = completer.get_completions();
}



/**
   Print the GNU longopt style switch \c opt, and the argument \c
   argument to the specified stringbuffer, but only if arguemnt is
   non-null and longer than 0 characters.
*/
static void append_switch(wcstring &out,
                          const wcstring &opt,
                          const wcstring &argument)
{
    if (argument.empty())
        return;

    wcstring esc = escape_string(argument, 1);
    append_format(out, L" --%ls %ls", opt.c_str(), esc.c_str());
}

wcstring complete_print()
{
    wcstring out;
    scoped_lock locker(completion_lock);

    // Get a list of all completions in a vector, then sort it by order
    std::vector<const completion_entry_t *> all_completions(completion_set.begin(), completion_set.end());
    sort(all_completions.begin(), all_completions.end(), compare_completions_by_order);

    for (std::vector<const completion_entry_t *>::const_iterator iter = all_completions.begin(); iter != all_completions.end(); ++iter)
    {
        const completion_entry_t *e = *iter;
        const legacy_option_list_t &options = e->options;
        for (legacy_option_list_t::const_iterator oiter = options.begin(); oiter != options.end(); ++oiter)
        {
            const legacy_option_t &o = *oiter;
            
            out.append(L"complete");
            
            if (! (o.metadata.tag & argument_allow_files))
            {
                out.append(L" --no-files");
            }

            append_switch(out,
                          e->cmd_is_path ? L"path" : L"command",
                          escape_string(e->cmd, ESCAPE_ALL));
        
            if (! o.option.empty())
            {
                switch (o.type)
                {
                    case legacy_option_t::single_short:
                        append_switch(out, L"short-option", o.option);
                        break;
                    case legacy_option_t::single_long:
                        append_switch(out, L"old-option", o.option);
                        break;
                    case legacy_option_t::double_long:
                        append_switch(out, L"long-option", o.option);
                        break;
                }
            }

            append_switch(out,
                          L"description",
                          C_(o.metadata.description));

            append_switch(out,
                          L"arguments",
                          o.metadata.command);

            append_switch(out,
                          L"condition",
                          o.metadata.condition);

            out.append(L"\n");
        }
    }
    
    /* Append wraps. This is a wonky interface where even values are the commands, and odd values are the targets that they wrap. */
    const wcstring_list_t wrap_pairs = complete_get_wrap_pairs();
    assert(wrap_pairs.size() % 2 == 0);
    for (size_t i=0; i < wrap_pairs.size(); )
    {
        const wcstring &cmd = wrap_pairs.at(i++);
        const wcstring &target = wrap_pairs.at(i++);
        append_format(out, L"complete --command %ls --wraps %ls\n", cmd.c_str(), target.c_str());
    }
    return out;
}


/* Completion "wrapper" support. The map goes from wrapping-command to wrapped-command-list */
static pthread_mutex_t wrapper_lock = PTHREAD_MUTEX_INITIALIZER;
typedef std::map<wcstring, wcstring_list_t> wrapper_map_t;
static wrapper_map_t &wrap_map()
{
    ASSERT_IS_LOCKED(wrapper_lock);
    // A pointer is a little more efficient than an object as a static because we can elide the thread-safe initialization
    static wrapper_map_t *wrapper_map = NULL;
    if (wrapper_map == NULL)
    {
        wrapper_map = new wrapper_map_t();
    }
    return *wrapper_map;
}

/* Add a new target that is wrapped by command. Example: __fish_sgrep (command) wraps grep (target). */
bool complete_add_wrapper(const wcstring &command, const wcstring &new_target)
{
    if (command.empty() || new_target.empty())
    {
        return false;
    }
    
    scoped_lock locker(wrapper_lock);
    wrapper_map_t &wraps = wrap_map();
    wcstring_list_t *targets = &wraps[command];
    // If it's already present, we do nothing
    if (std::find(targets->begin(), targets->end(), new_target) == targets->end())
    {
        targets->push_back(new_target);
    }
    return true;
}

bool complete_remove_wrapper(const wcstring &command, const wcstring &target_to_remove)
{
    if (command.empty() || target_to_remove.empty())
    {
        return false;
    }
    
    scoped_lock locker(wrapper_lock);
    wrapper_map_t &wraps = wrap_map();
    bool result = false;
    wrapper_map_t::iterator current_targets_iter = wraps.find(command);
    if (current_targets_iter != wraps.end())
    {
        wcstring_list_t *targets = &current_targets_iter->second;
        wcstring_list_t::iterator where = std::find(targets->begin(), targets->end(), target_to_remove);
        if (where != targets->end())
        {
            targets->erase(where);
            result = true;
        }
    }
    return result;
}

wcstring_list_t complete_get_wrap_chain(const wcstring &command)
{
    if (command.empty())
    {
        return wcstring_list_t();
    }
    scoped_lock locker(wrapper_lock);
    const wrapper_map_t &wraps = wrap_map();
    
    wcstring_list_t result;
    std::set<wcstring> visited; // set of visited commands
    wcstring_list_t to_visit(1, command); // stack of remaining-to-visit commands
    
    wcstring target;
    while (! to_visit.empty())
    {
        // Grab the next command to visit, put it in target
        target.swap(to_visit.back());
        to_visit.pop_back();
        
        // Try inserting into visited. If it was already present, we skip it; this is how we avoid loops.
        if (! visited.insert(target).second)
        {
            continue;
        }
        
        // Insert the target in the result. Note this is the command itself, if this is the first iteration of the loop.
        result.push_back(target);
        
        // Enqueue its children
        wrapper_map_t::const_iterator target_children_iter = wraps.find(target);
        if (target_children_iter != wraps.end())
        {
            const wcstring_list_t &children = target_children_iter->second;
            to_visit.insert(to_visit.end(), children.begin(), children.end());
        }
    }
    
    return result;
}

wcstring_list_t complete_get_wrap_pairs()
{
    wcstring_list_t result;
    scoped_lock locker(wrapper_lock);
    const wrapper_map_t &wraps = wrap_map();
    for (wrapper_map_t::const_iterator outer = wraps.begin(); outer != wraps.end(); ++outer)
    {
        const wcstring &cmd = outer->first;
        const wcstring_list_t &targets = outer->second;
        for (size_t i=0; i < targets.size(); i++)
        {
            result.push_back(cmd);
            result.push_back(targets.at(i));
        }
    }
    return result;
}
