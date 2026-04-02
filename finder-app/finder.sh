#!/bin/sh

usage="$0 <filesdir> <searchstr>"

filesdir=$1
searchstr=$2

if [ ! $# -eq 2 ]
then
    echo "Wrong number of arguments given. Usage: ${usage}."
    exit 1
fi

if [ -z ${filesdir} ] || [ -z ${searchstr} ]
then
    echo "At least one parameter was not specified. Usage: ${usage}."
    exit 1
fi

if [ ! -d ${filesdir} ]
then
    echo "Parameter ${filesdir} does not specify an existing directory. Usage: ${usage}."
    exit 1
fi

files=$(find ${filesdir} -type f | wc -l)
matching_lines=$(egrep ${searchstr} ${filesdir} -r | wc -l)

echo "The number of files are ${files} and the number of matching lines are ${matching_lines}."
exit 0