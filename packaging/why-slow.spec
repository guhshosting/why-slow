Name:           why-slow
Version:        1.0.0
Release:        1%{?dist}
Summary:        Read-only Linux performance observations
License:        MIT
Source0:        why-slow-%{version}.tar.gz
BuildRequires:  gcc-c++
BuildRequires:  cmake >= 3.16
BuildRequires:  make
BuildRequires:  python3

%description
Collect local Linux performance observations with conservative explanations.
No telemetry, network access or automatic optimization.

%prep
%setup -q

%build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=%{_prefix} -DCMAKE_INSTALL_LIBDIR=%{_lib}
cmake --build build --parallel 2

%check
ctest --test-dir build --output-on-failure

%install
DESTDIR=%{buildroot} cmake --install build

%files
%license LICENSE
%{_bindir}/why-slow
%{_mandir}/man1/why-slow.1*
%{_docdir}/why-slow/

%changelog
* Wed Sep 16 2026 why-slow maintainers - 1.0.0-1
- Initial release
