#!/bin/sh

DEBRELEASE=$(head -n1 debian/changelog | cut -d ' ' -f 2 | sed 's/[()]*//g')

TMPDIR=/tmp/jmc-${DEBRELEASE}
rm -rf ${TMPDIR}

GITBRANCH=${GITBRANCH:-master}

echo "debian -export-ignore" >> .git/info/attributes

git-buildpackage \
	--git-upstream-branch=$GITBRANCH --git-debian-branch=$GITBRANCH \
	--git-upstream-tree=branch \
	--git-export-dir=${TMPDIR} --git-cleaner=/bin/true \
	--git-force-create \
	-rfakeroot $@ 

ERROR=$?

ed -s .git/info/attributes > /dev/null << EOF
/debian -export-ignore
d
wq
EOF

if test $ERROR != 0; then
	exit $ERROR
fi

lintian -i --pedantic ${TMPDIR}/jmc_${DEBRELEASE}_*.changes \
	| tee /tmp/jmc.issues

echo
ls ${TMPDIR}/jmc_${DEBRELEASE}_*.changes
echo
echo dput rg42 ${TMPDIR}/jmc_${DEBRELEASE}_*.changes
