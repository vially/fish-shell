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
	
		# Bail out early if the exact abbr is already in
		# This depends on the separator staying the same, but that's the common case (config.fish)
		contains -- "$word_phrase" $fish_user_abbreviations; and return 0
	
		set -l kv (__fish_abbr_split "$word_phrase")
		set -l key $kv[1]
		set -l value $kv[2]

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
		set -l key (__fish_abbr_split "$word")[1]
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
			set -l kv (__fish_abbr_split $i)
			set -l key $kv[1]
			set -l value $kv[2]

			# Check to see if either key or value has a leading dash
			# If so, we need to write --
			string match -q -- '-*' $key $value; and set -l opt_double_dash '--'
			echo abbr $opt_double_dash (string escape -- $key $value)
		end
		return 0

	else if set -ql opt_list
		for i in $fish_user_abbreviations
			set -l key (__fish_abbr_split $i)[1]
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
	# Going through all entries is still quicker than calling `seq`
	set -l keys
	for kv in $fish_user_abbreviations
		if string match -qr '^[^ ]+=' -- $kv
			# No need for bounds-checking because we already matched before
			set keys $keys (string split "=" -m 1 -- $kv)[1]
		else if string match -qr '^[^ ]+ .*' -- $kv
			set keys $keys (string split " " -m 1 -- $kv)[1]
		end
	end
	if set -l idx (contains -i -- $argv[1] $keys)
		echo $idx
		return 0
	end
	return 1
end

function __fish_abbr_split -a input
	if string match -qr '^[^ ]+=' -- $input
		string split "=" -m 1 -- $input
	else if string match -qr '^[^ ]+ .*' -- $input
		string split " " -m 1 -- $input
	else
		echo $input
	end
end
