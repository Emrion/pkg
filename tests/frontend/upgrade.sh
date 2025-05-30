#! /usr/bin/env atf-sh

. $(atf_get_srcdir)/test_environment.sh

tests_init \
	issue1881 \
	issue1881_newdep \
	three_digit_revision \
	dual_conflict \
	file_become_dir \
	dir_become_file \
	dir_is_symlink_to_a_dir \
	vital \
	vital_force \
	vital_force_cant_remove

issue1881_body() {
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg pkg1 pkg_a 1
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg pkg2 pkg_a 1_1

	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg pkg3 pkg_b 1
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg pkg4 pkg_b 1_1

	atf_check \
		-o match:".*Installing.*\.\.\.$" \
		-e empty \
		-s exit:0 \
		pkg register -M pkg1.ucl

	atf_check \
		-o ignore \
		-e empty \
		-s exit:0 \
		pkg create -M ./pkg3.ucl

	atf_check \
		-o ignore \
		-e empty \
		-s exit:0 \
		pkg create -M ./pkg2.ucl

	atf_check \
		-o ignore \
		-e empty \
		-s exit:0 \
		pkg create -M ./pkg4.ucl

	atf_check \
		-o inline:"Creating repository in .:  done\nPacking files for repository:  done\n" \
		-e empty \
		-s exit:0 \
		pkg repo .

	mkdir repoconf
	cat << EOF > repoconf/repo.conf
local: {
	url: file:///$TMPDIR,
	enabled: true
}
EOF

	atf_check \
		-o not-match:"^[[:space:]]+pkg_b: 1$" \
		-e ignore \
		-s exit:0 \
		pkg -o REPOS_DIR="$TMPDIR/repoconf" -o PKG_CACHEDIR="$TMPDIR" upgrade -yx '^pkg_'
}

issue1881_newdep_body() {
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg pkg1 pkg_a 1
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg pkg2 pkg_a 1_1

	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg pkg3 pkg_b 1
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg pkg4 pkg_b 1_1

	cat <<EOF >> ./pkg2.ucl
deps: {
	pkg_b: {
		origin: "wedontcare",
		version: "1"
	}
}
EOF

	atf_check \
		-o match:".*Installing.*\.\.\.$" \
		-e empty \
		-s exit:0 \
		pkg register -M pkg1.ucl

	atf_check \
		-o ignore \
		-e empty \
		-s exit:0 \
		pkg create -M ./pkg3.ucl

	atf_check \
		-o ignore \
		-e empty \
		-s exit:0 \
		pkg create -M ./pkg2.ucl

	atf_check \
		-o ignore \
		-e empty \
		-s exit:0 \
		pkg create -M ./pkg4.ucl

	atf_check \
		-o inline:"Creating repository in .:  done\nPacking files for repository:  done\n" \
		-e empty \
		-s exit:0 \
		pkg repo .

	mkdir repoconf
	cat << EOF > repoconf/repo.conf
local: {
	url: file:///$TMPDIR,
	enabled: true
}
EOF

	atf_check \
		-o match:"^[[:space:]]+pkg_b: 1_1$" \
		-e ignore \
		-s exit:0 \
		pkg -o REPOS_DIR="$TMPDIR/repoconf" -o PKG_CACHEDIR="$TMPDIR" upgrade -yx '^pkg_'
}

three_digit_revision_body() {

	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg pkg1 pkg_a 1_90

	atf_check \
		-o ignore \
		-e empty \
		-s exit:0 \
		pkg register -M pkg1.ucl

	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg pkg1 pkg_a 1_125

	atf_check \
		-o ignore \
		-e empty \
		-s exit:0 \
		pkg create -M ./pkg1.ucl

	atf_check \
		-o ignore \
		-e empty \
		-s exit:0 \
		pkg repo .

	mkdir repoconf
	cat << EOF > repoconf/repo.conf
local: {
	url: file:///$TMPDIR,
	enabled: true
}
EOF

	atf_check \
		-o ignore \
		-e empty \
		-s exit:0 \
		pkg -o REPOS_DIR="$TMPDIR/repoconf" -o PKG_CACHEDIR="$TMPDIR" upgrade -yx '^pkg_'
	atf_check \
		-o inline:"pkg_a-1_125\n" \
		-e empty \
		-s exit:0 \
		pkg info -q
}

dual_conflict_body()
{
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg "pkg-1" "pkg-1" "1"
	echo "${TMPDIR}/file-pkg-1" > plist-1
	echo "entry" > file-pkg-1
	atf_check -s exit:0 pkg create -M pkg-1.ucl -p plist-1

	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg "pkg-2" "pkg-2" "1"
	echo "${TMPDIR}/file-pkg-2" > plist-2
	echo "entry" > file-pkg-2
	atf_check -s exit:0 pkg create -M pkg-2.ucl -p plist-2

	mkdir repoconf
	cat << EOF > repoconf/repo.conf
local: {
	url: file:///$TMPDIR,
	enabled: true
}
EOF

	atf_check \
		-o inline:"Creating repository in .:  done\nPacking files for repository:  done\n" \
		-e empty \
		-s exit:0 \
		pkg repo .

	mkdir ${TMPDIR}/target
	atf_check \
		pkg -o REPOS_DIR="$TMPDIR/repoconf" -o PKG_CACHEDIR="$TMPDIR" -r ${TMPDIR}/target install -qy pkg-1 pkg-2
	pkg -r ${TMPDIR}/target which ${TMPDIR}/file-pkg-1
	pkg -r ${TMPDIR}/target which ${TMPDIR}/file-pkg-2
	test -f ${TMPDIR}/target/${TMPDIR}/file-pkg-1 || atf_fail "file absent"
	test -f ${TMPDIR}/target/${TMPDIR}/file-pkg-2 || atf_fail "file absent"

	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg "pkg-1" "pkg-1" "2"
	echo "${TMPDIR}/file-pkg-2" > plist-1
	atf_check -s exit:0 pkg create -M pkg-1.ucl -p plist-1

	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg "pkg-2" "pkg-2" "2"
	echo "${TMPDIR}/file-pkg-1" > plist-2
	atf_check -s exit:0 pkg create -M pkg-2.ucl -p plist-2

	sleep 1
	atf_check \
		-o inline:"Creating repository in .:  done\nPacking files for repository:  done\n" \
		-e empty \
		-s exit:0 \
		pkg repo -l .

	atf_check \
		pkg -o REPOS_DIR="$TMPDIR/repoconf" -o PKG_CACHEDIR="$TMPDIR" -r ${TMPDIR}/target update -q

	atf_check \
		pkg -o REPOS_DIR="$TMPDIR/repoconf" -o PKG_CACHEDIR="$TMPDIR" -r ${TMPDIR}/target upgrade -qy

	atf_check \
		-o inline:'pkg-2-2\n' \
		pkg -r ${TMPDIR}/target which -q ${TMPDIR}/file-pkg-1
	atf_check \
		-o inline:'pkg-1-2\n' \
		pkg -r ${TMPDIR}/target which -q ${TMPDIR}/file-pkg-2
}

file_become_dir_body() {
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg "pkg" "pkg" "1"
	echo "${TMPDIR}/file-pkg-1" > plist-1
	echo "entry" > file-pkg-1
	atf_check pkg create -M pkg.ucl -p plist-1
	mkdir target
	atf_check -o ignore pkg -o REPOS_DIR="${TMPDIR}" -r ${TMPDIR}/target install -Uy ${TMPDIR}/pkg-1.pkg
	atf_check test -f target/${TMPDIR}/file-pkg-1
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg "pkg" "pkg" "2"
	rm file-pkg-1
	mkdir file-pkg-1
	echo entry > file-pkg-1/file
	echo "${TMPDIR}/file-pkg-1/file" > plist-2
	atf_check pkg create -M pkg.ucl -p plist-2
	atf_check -o ignore pkg -o REPOS_DIR="${TMPDIR}" -r ${TMPDIR}/target install -Uy ${TMPDIR}/pkg-2.pkg
}

dir_become_file_body() {
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg "pkg" "pkg" "1"
	mkdir file-pkg-1
	echo entry > file-pkg-1/file
	echo "${TMPDIR}/file-pkg-1/file" > plist-1
	atf_check pkg create -M pkg.ucl -p plist-1
	mkdir target
	atf_check -o ignore pkg -o REPOS_DIR="${TMPDIR}" -r ${TMPDIR}/target install -Uy ${TMPDIR}/pkg-1.pkg
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg "pkg" "pkg" "2"
	rm -rf file-pkg-1
	echo entry > file-pkg-1
	echo "${TMPDIR}/file-pkg-1" > plist-2
	atf_check pkg create -M pkg.ucl -p plist-2
	atf_check -o ignore pkg -o REPOS_DIR="${TMPDIR}" -r ${TMPDIR}/target install -Uy ${TMPDIR}/pkg-2.pkg
}

dir_is_symlink_to_a_dir_body()
{
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg "pkg" "pkg" "1"
	mkdir share lib lib/something
	ln -sf ../lib/something share/something
	echo "entry" > lib/something/file
	echo "${TMPDIR}/lib/something/file" > plist-1
	echo "${TMPDIR}/share/something" >> plist-1
	atf_check pkg create -M pkg.ucl -p plist-1
	mkdir target
	atf_check -o ignore pkg -o REPOS_DIR="${TMPDIR}" -r ${TMPDIR}/target install -Uy ${TMPDIR}/pkg-1.pkg
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg "pkg" "pkg" "2"
	rm share/something
	mkdir share/something
	echo "entry" > share/something/file
	echo "${TMPDIR}/lib/something/file" > plist-2
	echo "${TMPDIR}/share/something/file" >> plist-2
	atf_check pkg create -M pkg.ucl -p plist-2
	atf_check -o ignore pkg -o REPOS_DIR="${TMPDIR}" -r ${TMPDIR}/target install -Uy ${TMPDIR}/pkg-2.pkg
}

vital_body() {
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg "meta" "mymeta" "1"
	mkdir file-pkg-1
	cat << EOF >> meta.ucl
vital = true;
EOF
	echo entry > file-pkg-1/file
	echo "${TMPDIR}/file-pkg-1/file" > plist-1
	atf_check pkg create -M meta.ucl -p plist-1
	mkdir target
	atf_check -o ignore pkg -o REPOS_DIR="${TMPDIR}" -r ${TMPDIR}/target install -Uy ${TMPDIR}/mymeta-1.pkg
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg "plop" "myplop" "1"
	atf_check pkg create -M plop.ucl
	atf_check -o ignore pkg -o REPOS_DIR="${TMPDIR}" -r ${TMPDIR}/target install -Uy ${TMPDIR}/myplop-1.pkg
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg "plop" "myplop" "2"
	echo "${TMPDIR}/file-pkg-1/file" > plist-2
	atf_check pkg create -M plop.ucl -p plist-2
	mkdir repoconf
	cat << EOF > repoconf/repo.conf
local: {
	url: file:///$TMPDIR,
	enabled: true
}
EOF

	atf_check -o ignore pkg repo .
	atf_check -o ignore pkg -o REPOS_DIR="$TMPDIR/repoconf" -r ${TMPDIR}/target -o PKG_CACHEDIR="$TMPDIR" update
	OUTPUT="Updating local repository catalogue...
local repository is up to date.
All repositories are up to date.
Checking for upgrades (2 candidates):  done
Processing candidates (2 candidates):  done
Checking integrity... done (1 conflicting)
  - myplop-2 conflicts with mymeta-1 on ${TMPDIR}/file-pkg-1/file
Cannot solve problem using SAT solver, trying another plan
Checking integrity... done (0 conflicting)
Your packages are up to date.
"
	ERROR=""
	atf_check -o inline:"${OUTPUT}" -e inline:"${ERROR}" -s exit:0 pkg -o REPOS_DIR="$TMPDIR/repoconf" -r ${TMPDIR}/target -o PKG_CACHEDIR="$TMPDIR" upgrade -y
}

vital_force_body() {
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg "meta" "mymeta" "1"
	mkdir file-pkg-1
	cat << EOF >> meta.ucl
vital = true;
EOF
	echo entry > file-pkg-1/file
	echo "${TMPDIR}/file-pkg-1/file" > plist-1
	atf_check pkg create -M meta.ucl -p plist-1
	mkdir target
	atf_check -o ignore pkg -o REPOS_DIR="${TMPDIR}" -r ${TMPDIR}/target install -Uy ${TMPDIR}/mymeta-1.pkg
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg "plop" "myplop" "1"
	atf_check pkg create -M plop.ucl
	atf_check -o ignore pkg -o REPOS_DIR="${TMPDIR}" -r ${TMPDIR}/target install -Uy ${TMPDIR}/myplop-1.pkg
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg "plop" "myplop" "2"
	echo "${TMPDIR}/file-pkg-1/file" > plist-2
	atf_check pkg create -M plop.ucl -p plist-2
	mkdir repoconf
	cat << EOF > repoconf/repo.conf
local: {
	url: file:///$TMPDIR,
	enabled: true
}
EOF

	atf_check -o ignore pkg repo .
	atf_check -o ignore pkg -o REPOS_DIR="$TMPDIR/repoconf" -r ${TMPDIR}/target -o PKG_CACHEDIR="$TMPDIR" update
	OUTPUT="Updating local repository catalogue...
local repository is up to date.
All repositories are up to date.
Checking integrity... done (1 conflicting)
  - myplop-2 conflicts with mymeta-1 on ${TMPDIR}/file-pkg-1/file
Checking integrity... done (0 conflicting)
The following 2 package(s) will be affected (of 0 checked):

Installed packages to be UPGRADED:
	myplop: 1 -> 2

Installed packages to be REMOVED:
	mymeta: 1

Number of packages to be removed: 1
Number of packages to be upgraded: 1
[1/2] Deinstalling mymeta-1...
[1/2] Deleting files for mymeta-1:  done
[2/2] Upgrading myplop from 1 to 2...
[2/2] Extracting myplop-2:  done
"
	ERROR=""
	atf_check -o inline:"${OUTPUT}" -e inline:"${ERROR}" -s exit:0 pkg -o REPOS_DIR="$TMPDIR/repoconf" -r ${TMPDIR}/target -o PKG_CACHEDIR="$TMPDIR" upgrade -fy myplop-2
}

vital_force_cant_remove_body() {
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg "meta" "mymeta" "1"
	mkdir file-pkg-1
	cat << EOF >> meta.ucl
vital = true;
EOF
	echo entry > file-pkg-1/file
	echo "${TMPDIR}/file-pkg-1/file" > plist-1
	atf_check pkg create -M meta.ucl -p plist-1
	mkdir target
	atf_check -o ignore pkg -o REPOS_DIR="${TMPDIR}" -r ${TMPDIR}/target install -Uy ${TMPDIR}/mymeta-1.pkg
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg "plop" "myplop" "1"
	atf_check pkg create -M plop.ucl
	atf_check -o ignore pkg -o REPOS_DIR="${TMPDIR}" -r ${TMPDIR}/target install -Uy ${TMPDIR}/myplop-1.pkg
	atf_check -s exit:0 sh ${RESOURCEDIR}/test_subr.sh new_pkg "plop" "myplop" "2"
	echo "${TMPDIR}/file-pkg-1/file" > plist-2
	atf_check pkg create -M plop.ucl -p plist-2
	mkdir repoconf
	cat << EOF > repoconf/repo.conf
local: {
	url: file:///$TMPDIR,
	enabled: true
}
EOF

	atf_check -o ignore pkg repo .
	atf_check -o ignore pkg -o REPOS_DIR="$TMPDIR/repoconf" -r ${TMPDIR}/target -o PKG_CACHEDIR="$TMPDIR" update
	OUTPUT="Updating local repository catalogue...
local repository is up to date.
All repositories are up to date.
Checking integrity... done (1 conflicting)
  - myplop-2 conflicts with mymeta-1 on ${TMPDIR}/file-pkg-1/file
Cannot solve problem using SAT solver, trying another plan
Checking integrity... done (0 conflicting)
Your packages are up to date.
"
	ERROR=""
	atf_check -o inline:"${OUTPUT}" -e inline:"${ERROR}" -s exit:0 pkg -o REPOS_DIR="$TMPDIR/repoconf" -r ${TMPDIR}/target -o PKG_CACHEDIR="$TMPDIR" -o FORCE_CAN_REMOVE_VITAL=NO upgrade -fy myplop-2
}
