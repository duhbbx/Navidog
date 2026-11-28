# ScriptDOM offline parser probe

Run `python3 scripts/collect-sql-native-parsers.py` from the repository root to
fetch the pinned Microsoft package and verify both its SHA-512 and assembly
SHA-256. The package stays in `.cache/`, including its original NuGet metadata
and MIT license declaration. It is not linked into Navidog.

Build with the installed .NET 8 SDK:

```sh
dotnet build scripts/sql-grammar/sqlserver-native/SqlServerProbe.csproj \
  --configuration Release --artifacts-path .cache/sqlserver-native-parser/artifacts \
  --configfile scripts/sql-grammar/sqlserver-native/NuGet.Config
```

The project uses a direct reference to the verified cache file and no NuGet
package sources. Keep CLI home/build caches in a disposable directory when
reproducing the audit. Run the resulting `SqlServerProbe.dll --version` with
`dotnet` to report the exact library hash and supported parser versions.

The normal protocol is one JSON request per line (`index`, `sql`, optional
`version`, optional `quoted_identifiers`) and one JSON recognition result per
line. The default profile is T-SQL 160. ScriptDOM creates ASTs and reports its
native parser checks; no SQL Server client or database connection is used.
