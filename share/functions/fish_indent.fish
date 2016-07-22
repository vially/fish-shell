function fish_indent --description 'Indenter and prettifier for fish code' --signature '
  Usage:
      fish_indent [--no-indent] [--ansi | --html]
      fish_indent --version
      fish_indent --help
  Options:
      -h, --help       Display help and exit
      -v, --version    Display version and exit
      -i, --no-indent  Do not indent output, only reformat into one job per line
      --ansi           Colorize the output using ANSI escape sequences
      --html           Output in HTML format
'
	# This is wrapped in a function so that fish_indent does not need to be found in PATH
	eval (string escape $__fish_bin_dir/fish_indent) $argv
end
