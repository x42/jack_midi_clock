#!/bin/bash

NEWVERSION=$1

if test -n "$(echo "$NEWVERSION" | sed 's/^[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*$//')"; then
	NEWVERSION=""
fi

if test -z "$NEWVERSION"; then
	echo "No X.X.X version given as argument."
	exit 1
fi

echo "commit pending changes.."
git commit -a

dch --newversion "${NEWVERSION}-0.1" --distribution unstable || exit
sed -i 's/echo "v[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*"/echo "v'$NEWVERSION'"/' Makefile
vi ChangeLog

make VERSION="v${NEWVERSION}" clean man || exit

git status -s
echo " - Version v${NEWVERSION}"

echo -n "git commit and tag? [Y/n]"
read -n1 a
echo
if test "$a" == "n" -o "$a" == "N"; then
	exit 1
fi

git commit -m "finalize changelog" \
	debian/changelog Makefile ChangeLog \
	jack_mclk_dump.1 jack_midi_clock.1
git tag "v${NEWVERSION}"

echo -n "git push? [Y/n] "
read -n1 a
echo
if test "$a" == "n" -o "$a" == "N"; then
	exit 1
fi

git push github && git push github --tags
git push rg42 && git push rg42 --tags
