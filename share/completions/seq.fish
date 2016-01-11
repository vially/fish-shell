if seq --version ^ /dev/null > /dev/null #GNU

complete --signature '
    Usage:   seq [options] [<first> [<incr>]] <last>
    Options:
        -f, --format <format>     Use printf style floating-point format
        -s, --separator <string>  Use string to separate numbers
        -w, --equal-width         Equalize width with leading zeroes
        --help                    Display help
        --version                 Output version information
'
else #OS X

complete --signature '
    Usage:   seq [options] [<first> [<incr>]] <last>
    Options:
        -f <format>     Use printf style floating-point format
        -s <string>     Use string to separate numbers
		-t <string>     Use string to terminate the sequence
        -w              Equalize width with leading zeroes
        -h              Display help
'
end
