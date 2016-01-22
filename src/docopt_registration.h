/** \file docopt_registration.h
  Support for registering docopt descriptions of commands and functions
*/

#ifndef FISH_DOCOPT_REGISTRATION_H
#define FISH_DOCOPT_REGISTRATION_H

#include <wchar.h>

#include "util.h"
#include "common.h"
#include "io.h"
#include "docopt_fish.h"
#include <vector>
#include <map>

struct parse_error_t;
typedef std::vector<parse_error_t> parse_error_list_t;

/* Covers for docopt functions */
enum docopt_argument_status_t {
    status_invalid, // the argument doesn't work
    status_valid, // the argument works fine
    status_valid_prefix // the argument is a prefix of something that may work
};

enum docopt_parse_flags_t {
    flags_default = 0U,
    flag_generate_empty_args = 1U << 0,
    flag_match_allow_incomplete = 1U << 1,
    flag_resolve_unambiguous_prefixes = 1U << 2,
};

/* Metadata for docopt entities. This may refer to options, variables, or literals. */
struct docopt_metadata_t
{
    // The command that generates the value of arguments
    wcstring command;
    
    // The condition required for this option to be used
    wcstring condition;
    
    // The description of the option
    wcstring description;
};

/* A handle on a docopt registration, which can be used for removal. 0 is guaranteed invalid. */
typedef unsigned long long docopt_registration_handle_t;

/* Ugly forward declaration */
namespace docopt_fish
{
    template<typename T>
    class argument_parser_t;
}

class docopt_arguments_t;
class docopt_registration_t;

/* Represents a set of docopt registrations */
class docopt_registration_set_t
{
    friend class doc_register_t;
    std::vector<shared_ptr<const docopt_registration_t> > registrations;
    
public:
    
    /* Given a command and proposed arguments for the command, return a vector of equal size containing a status for each argument. Returns an empty vector if we have no validation information. */
    std::vector<docopt_argument_status_t> validate_arguments(const wcstring_list_t &argv, docopt_parse_flags_t flags = flags_default) const;
    
    /* Given a command and proposed arguments for the command, return a list of suggested next arguments */
    wcstring_list_t suggest_next_argument(const wcstring_list_t &argv, docopt_parse_flags_t flags = flags_default) const;
    
    /* Given some name (variable, option, or literal), return metadata for it. */
    docopt_metadata_t metadata_for_name(const wcstring &name) const;
        
    /* Given a command and a list of arguments, parses it into an argument list. Returns by reference: a map from argument name to value, a list of errors, and a list of unused arguments. If there is no docopt registration, the result is false. */
    bool parse_arguments(const wcstring_list_t &argv, docopt_arguments_t *out_arguments, parse_error_list_t *out_errors, std::vector<size_t> *out_unused_arguments) const;
    
    bool empty() const
    {
        return this->registrations.empty();
    }
    
    ~docopt_registration_set_t();
};

/* Helper class for representing the result of parsing argv via docopt. */
class docopt_arguments_t
{
    friend class docopt_registration_set_t;
    // The map from key to value
    std::map<wcstring, wcstring_list_t> vals;
    
    const wcstring_list_t *get_list_internal(const wcstring &key) const;
    
    public:
    void swap(docopt_arguments_t &rhs);
    
    /* Returns true if there is a value for the given key */
    bool has(const wchar_t *) const;
    bool has(const wcstring &) const;
    
    /* Returns number of arguments */
    size_t size() const
    {
        return vals.size();
    }
    
    /* Returns the value dictionary */
    const std::map<wcstring, wcstring_list_t> &values() const
    {
        return vals;
    }
    
    /* Returns the array of values for a given key, or an empty list if none */
    const wcstring_list_t &get_list(const wchar_t *) const;
    
    /* Returns the first value for a given key, or an empty string if none */
    const wcstring &get(const wchar_t *) const;
    
    /* Returns the first value for a given key, or NULL if none */
    const wchar_t *get_or_null(const wchar_t *) const;
    
    /* Helper function for "dumping" args to a string, for debugging */
    wcstring dump() const;
};

/* Entry points */

/** Given a command, name, usage spec, and description, register the usage. If cmd is empty, infers the command from the doc if there is only one, else returns an error.
 
 \param cmd The command for which to register the usage, or empty to infer it
 \param condition A conditition for when this usage should be active, or empty for none
 \param usage The actual docopt usage spec
 \param description A default description for completions generated from this usage spec
 \param out_errors Parse errors for the usage spec, returned by reference
 \param out_handle Returned handle to the docopt registration, for later removal.
 
 \return true on success, false on parse error
 */
bool docopt_register_usage(const wcstring &cmd, const wcstring &condition, const wcstring &usage, const wcstring &description, parse_error_list_t *out_errors, docopt_registration_handle_t *out_handle = NULL);


void docopt_register_direct_options(const wcstring &cmd, const std::vector<docopt_fish::base_annotated_option_t<wcstring> >& options, docopt_registration_handle_t *out_handle);

/** Get the set of registrations for a given command */
docopt_registration_set_t docopt_get_registrations(const wcstring &cmd);

/** Remove registratrion given a handle */
void docopt_unregister(docopt_registration_handle_t handle);

/** Given a key name like -b, derive the docopt variable name like opt_b suitable for setting in a function.
 
 The rules are:
 
   - Commands get cmd prepended.  git checkout -> cmd_checkout
   - Options get opt prepended. rm -r -> opt_r
   - Variables are used as-is. echo <stuff> -> stuff
*/
wcstring docopt_derive_variable_name(const wcstring &key);

#endif
