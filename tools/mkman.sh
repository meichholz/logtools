#!/bin/sh
# $Id: mkman.sh,v 1.1.1.1 2002/01/07 17:18:53 eichholz Exp $

DEST=$1
EXT=`echo -n $DEST | sed -e 's/.*\.//g'`
SRC=`basename $DEST .$EXT`
SRC=`echo -n $SRC.pod`

# echo -e "DEST=$DEST, SRC=$SRC, EXT=$EXT"

MAINVER=`cat RCS/rcs.mainversion`
SUBVER=`cat RCS/rcs.version`
RELEASE=`echo "Ver." "$MAINVER.$SUBVER"`

echo "manifying $SRC"

pod2man --section=$EXT --center="ME tools" --release="$RELEASE" $SRC >$DEST
