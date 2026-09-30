# Packaging

CMake install and CPack DEB/RPM are the primary tested package routes. Configure
with `-DCMAKE_INSTALL_PREFIX=/usr`, build and run CTest, then use `cpack -G DEB`
or `cpack -G RPM` in the build directory. Generated packages belong outside the
source archive. Runtime payload is the executable, manual and documentation.
There are no postinst/preinst/postun/service hooks. Do not install builder-created
Ubuntu binaries on Debian as a substitute for building against Debian's runtime.

The native Debian recipe is provided for distribution builders: copy
`packaging/debian` to `debian`, supply your distribution maintainer address if required by local policy,
place an upstream `why-slow_1.0.0.orig.tar.gz` alongside the source, and run
`dpkg-buildpackage -us -uc` with the listed Build-Depends installed. Debhelper
uses dependency discovery and the package's test target. Source format is quilt.

The native RPM spec expects `why-slow-1.0.0.tar.gz` containing a top-level
`why-slow-1.0.0` directory in the rpmbuild SOURCES directory. BuildRequires describe
a normal RPM build environment. `rpmbuild -ba packaging/why-slow.spec` runs the
Release build and tests. CPack is a separate convenient package generation route.

See VALIDATION.md for the exact routes built/inspected in this environment;
provided recipes are not automatically claims of native distro validation.
