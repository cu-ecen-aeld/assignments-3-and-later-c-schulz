#!/bin/sh

usage="$0 <writefile> <writestr>"

writefile=$1
writestr=$2

if [ ! $# -eq 2 ]
then
    echo "Wrong number of arguments given. Usage: ${usage}."
    exit 1
fi

if [ -z ${writefile} ] || [ -z ${writestr} ]
then
    echo "At least one parameter was not specified. Usage: ${usage}."
    exit 1
fi

mkdir -p $(dirname ${writefile})
echo "${writestr}" > ${writefile}

exit 0