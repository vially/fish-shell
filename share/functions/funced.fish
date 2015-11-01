function funced --description 'Edit function definition' --signature '
    Usage:
      funced [-i | -e <editor>] <funcname>
      funced -h
    Options:
      -e, --editor <editor>   Specify the text editor to use
      -i, --interactive       Open function body in the built‐in editor
      -h, --help              Show help and usage information
	Arguments:
	  <funcname> (functions -na)
	  <editor>   (__fish_complete_command)
'
	if set -ql opt_help
		__fish_print_help funced
		return 0
	end

	# Determine the editor
	set -l effective_editor
	if set -ql editor
		set effective_editor $editor
	else if set -ql opt_interactive
		set effective_editor fish
	else if set -q VISUAL
		set effective_editor $VISUAL
	else if set -q EDITOR
		set effective_editor $EDITOR
	end
	
	if not set -ql funcname
        set_color red
        _ "funced: You must specify one function name
"
        set_color normal
        return 1
	end

    set -l init
    switch $funcname
        case '-*'
        set init function -- $funcname\n\nend
        case '*'
        set init function $funcname\n\nend
    end

    # Break effective_editor up to get its first command (i.e. discard flags)
    if test -n "$effective_editor"
        set -l effective_editor_cmd
        eval set editor_cmd $effective_editor
        if not type -q -f "$editor_cmd[1]"
            _ "funced: The value for \$EDITOR '$effective_editor' could not be used because the command '$editor_cmd[1]' could not be found
    "
            set effective_editor fish
        end
    end
    
    # If no editor is specified, use fish
    if test -z "$effective_editor"
        set effective_editor fish
    end

    if test "$effective_editor" = fish
        set -l IFS
        if functions -q -- $funcname
            # Shadow IFS here to avoid array splitting in command substitution
            set init (functions -- $funcname | fish_indent --no-indent)
        end

        set -l prompt 'printf "%s%s%s> " (set_color green) '$funcname' (set_color normal)'
        # Unshadow IFS since the fish_title breaks otherwise
        set -e IFS
        if read -p $prompt -c "$init" -s cmd
            # Shadow IFS _again_ to avoid array splitting in command substitution
            set -l IFS
            eval (echo -n $cmd | fish_indent)
        end
        return 0
    end

    set tmpname (mktemp -t fish_funced.XXXXXXXXXX.fish)

    if functions -q -- $funcname
        functions -- $funcname > $tmpname
    else
        echo $init > $tmpname
    end
        # Repeatedly edit until it either parses successfully, or the user cancels
        # If the editor command itself fails, we assume the user cancelled or the file
        # could not be edited, and we do not try again
        while true
            if not eval $effective_editor $tmpname
                        _ "Editing failed or was cancelled"
                        echo
                else
                if not source $tmpname
                                # Failed to source the function file. Prompt to try again.
                                echo # add a line between the parse error and the prompt
                                set -l repeat
                                set -l prompt (_ 'Edit the file again\? [Y/n]')
                                while test -z "$repeat"
                                        read -p "echo $prompt\  " repeat
                                end
                                if not contains $repeat n N no NO No nO
                                        continue
                                end
                                _ "Cancelled function editing"
                                echo
                        end
                end
                break
        end
    set -l stat $status
    rm -f $tmpname >/dev/null
    return $stat
end
