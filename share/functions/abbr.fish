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
    -s, --show               Print all abbreviations
    -l, --list               Print all abbreviation names
    -e, --erase <word>       Erase an abbreviation by name
    -h, --help               Show help
	
  Arguments:
	<word>    (abbr -s | cut -d" " -f 2- | sed -e "s/ /	/")
'
	# Note that if we are run with no arguments, we treat it like --add if
	# we have a word_phrase, and --show if we do not
	if set -q word_phrase
		set -l key
		set -l value
		__fish_abbr_parse_entry "$word_phrase" key value
		# ensure the key contains at least one non-space character
		if not string match -qr "\w" -- $key
			printf ( _ "%s: abbreviation must have a non-empty key\n" ) abbr >&2
			return 1
		end
		if not string match -qr "\w" -- $value
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
			__fish_abbr_parse_entry $i key value

			# Check to see if either key or value has a leading dash
			# If so, we need to write --
			set -l opt_double_dash ''
			switch $key ; case '-*'; set opt_double_dash ' --'; end
			switch $value ; case '-*'; set opt_double_dash ' --'; end
			echo abbr$opt_double_dash (string escape -- $key $value)
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

function __fish_abbr_get_by_key
	if not set -q argv[1]
		echo "__fish_abbr_get_by_key: expected one argument, got none" >&2
		return 2
	end
	set -l count (count $fish_user_abbreviations)
	if test $count -gt 0
		set -l key $argv[1] # This assumes the key is valid and pre-parsed
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
	# A "=" _before_ any space - we only read the first possible separator
	# because the key can contain neither spaces nor "="
	if string match -qr '^[^ ]+=' -- $__input
		# No need for bounds-checking because we already matched before
		set -l KV (string split "=" -m 1 -- $__input)
		set $__key $KV[1]
		set $__value $KV[2]
	else if string match -qr '^[^ ]+ .*' -- $__input
		set -l KV (string split " " -m 1 -- $__input)
		set $__key $KV[1]
		set $__value $KV[2]
	else
		# This is needed for `erase` et al, where we want to allow passing a value
		set $__key $__input
	end
	return 0
end
