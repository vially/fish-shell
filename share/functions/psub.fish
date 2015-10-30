
function psub --description "Read from stdin into a file and output the filename. Remove the file when the command that called psub exits." --signature '
  Usage: psub [-f | --file]
	     psub [-h | --help]
  Options:
    -f, --file  Communicate using a regular file, not a named pipe
	-h, --help  Display help and exit
'
	set -l filename
	set -l funcname
	if set -lq opt_help
		__fish_print_help psub
		return 0
	end
	
	if not status --is-command-substitution
		echo psub: Not inside of command substitution >&2
		return 1
	end

	set -l TMPDIR $TMPDIR
	if test -z "$TMPDIR[1]"
		set TMPDIR /tmp
	end

	if not set -lq opt_file
		# Write output to pipe. This needs to be done in the background so
		# that the command substitution exits without needing to wait for
		# all the commands to exit
		set dir (mktemp -d "$TMPDIR[1]"/.psub.XXXXXXXXXX); or return
		set filename $dir/psub.fifo
		mkfifo $filename
		cat >$filename &
	else
		set filename (mktemp "$TMPDIR[1]"/.psub.XXXXXXXXXX)
		cat >$filename
	end

	# Write filename to stdout
	echo $filename

	# Find unique function name
	while true
		set funcname __fish_psub_(random);
		if not functions $funcname >/dev/null ^/dev/null
			break;
		end
	end

	# Make sure we erase file when caller exits
	function $funcname --on-job-exit caller --inherit-variable filename --inherit-variable funcname
		command rm $filename
		functions -e $funcname
	end

end
