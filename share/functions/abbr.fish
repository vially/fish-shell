function abbr --description 'Manage abbreviations' --signature '
  Usage:
    abbr -a <word_phrase>...
    abbr -s
    abbr -l
    abbr -e <word>
    abbr [<word_phrase>...]
    abbr -h
    
  Options:
    -a, --add <word_phrase>  Adds a new abbreviation
    -s, --show               Show all abbreviated words and their expanded
    -l, --list               List all abbreviated words
    -e, --erase <word>       Erase the abbreviation <word>
    -h, --help               Show help
'
	# Note that if we are run with no arguments, we treat it like --add if
	# we have a word_phrase, and --show if we do not
	if set -q word_phrase
		set -l key
		set -l value
		__fish_abbr_parse_entry "$word_phrase" key value
		# ensure the key contains at least one non-space character
		set -l IFS \n\ \t
		printf '%s' $key | read -lz key_ __
		if test -z "$key_"
			printf ( _ "%s: abbreviation must have a non-empty key\n" ) abbr >&2
			return 1
		end
		if test -z "$value"
			printf ( _ "%s: abbreviation must have a value\n" ) abbr >&2
			return 1
		end
		if set -l idx (__fish_abbr_get_by_key $key)
			# erase the existing abbreviation
			set -e fish_user_abbreviations[$idx]
		end
		if not set -q fish_user_abbreviations
			# initialize as a universal variable, so we can skip the -U later
			# and therefore work properly if someone sets this as a global variable
			set -U fish_user_abbreviations
		end
		set fish_user_abbreviations $fish_user_abbreviations "$word_phrase"
		return 0
	
	else if set -ql opt_erase
		set -l key
		__fish_abbr_parse_entry $word key
		if set -l idx (__fish_abbr_get_by_key $key)
			set -e fish_user_abbreviations[$idx]
			return 0
		else
			printf ( _ "%s: no such abbreviation '%s'\n" ) abbr $key >&2
			return 2
		end
 
	else if begin ; set -ql opt_show ; or not count $argv > /dev/null; end
		# Either --show is set, or $argv is empty, which is like --show
		for i in $fish_user_abbreviations
			# Disable newline splitting
			set -lx IFS ''
			__fish_abbr_parse_entry $i key value

			# Check to see if either key or value has a leading dash
			# If so, we need to write --
			set -l opt_double_dash ''
			switch $key ; case '-*'; set opt_double_dash ' --'; end
			switch $value ; case '-*'; set opt_double_dash ' --'; end
			echo abbr$opt_double_dash (__fish_abbr_escape "$key") (__fish_abbr_escape "$value")
		end
		return 0

	else if set -ql opt_list
		for i in $fish_user_abbreviations
			set -l key
			__fish_abbr_parse_entry $i key
			printf "%s\n" $key
		end
		return 0

	else
		# Return success if -help was asked for
		__fish_print_help abbr
		return set -l opt_help
	end
end

function __fish_abbr_escape
	# Prettify the common case: if everything is alphanumeric,
	# we do not need escapes.
	# Do this by deleting alnum characters, and check if there's anything left.
	# Note we need to preserve spaces, so spaces are not considered alnum
	if test -z (echo -n "$argv" | tr -d '[:alnum:]_')
		echo $argv
	else
		# Escape via single quotes
		# printf is nice for stripping the newline that sed outputs
		printf "'%s'" (echo -n $argv | sed -e s,\\\\,\\\\\\\\,g -e s,\',\\\\\',g)
	end
end

function __fish_abbr_get_by_key
	if not set -q argv[1]
		echo "__fish_abbr_get_by_key: expected one argument, got none" >&2
		return 2
	end
	set -l count (count $fish_user_abbreviations)
	if test $count -gt 0
		set -l key
		__fish_abbr_parse_entry $argv[1] key
		set -l IFS \n # ensure newline splitting is enabled
		for i in (seq $count)
			set -l key_i
			__fish_abbr_parse_entry $fish_user_abbreviations[$i] key_i
			if test "$key" = "$key_i"
				echo $i
				return 0
			end
		end
	end
	return 1
end

function __fish_abbr_parse_entry -S -a __input __key __value
	if test -z "$__key"
		set __key __
	end
	if test -z "$__value"
		set __value __
	end
	set -l IFS '= '
	switch $__input
	case '=*'
		# read will skip any leading ='s, but we don't want that
		set __input " $__input"
		set __key _
		set IFS '='
	case ' =*'
		set __key _
		set IFS '='
	end
	# use read -z to avoid splitting on newlines
	# I think we can safely assume there will be no NULs in the input
	printf "%s" $__input | read -z $__key $__value
	return 0
end
