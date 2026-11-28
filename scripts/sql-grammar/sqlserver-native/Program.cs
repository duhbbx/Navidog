// Offline recognition only. ScriptDOM builds syntax trees; this program has no
// SQL Server client and never connects to or executes SQL against a database.
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.SqlServer.TransactSql.ScriptDom;

var json = new JsonSerializerOptions
{
    PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
    PropertyNameCaseInsensitive = true,
    DefaultIgnoreCondition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull
};
var library = typeof(TSqlParser).Assembly;
if (args.Length == 1 && args[0] == "--version")
{
    Console.WriteLine(JsonSerializer.Serialize(new
    {
        Package = "microsoft.sqlserver.transactsql.scriptdom",
        Version = "180.154.0",
        AssemblyVersion = library.GetName().Version?.ToString(),
        InformationalVersion = library.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion,
        AssemblySha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(library.Location))).ToLowerInvariant(),
        ParserProfiles = new[] { 80, 90, 100, 110, 120, 130, 140, 150, 160, 170, 180 }
            .Where(version => library.GetType($"Microsoft.SqlServer.TransactSql.ScriptDom.TSql{version}Parser") != null)
    }, json));
    return;
}
if (args.Length != 0)
{
    Console.Error.WriteLine("usage: SqlServerProbe [--version]; JSON requests on stdin");
    Environment.ExitCode = 2;
    return;
}
string? line;
while ((line = Console.ReadLine()) != null)
{
    var result = new Response();
    try
    {
        var input = JsonSerializer.Deserialize<Request>(line, json) ?? throw new JsonException("empty request");
        result.Index = input.Index;
        result.ParserVersion = input.Version;
        if (line.Length > 2 * 1024 * 1024 || Encoding.UTF8.GetByteCount(input.Sql) > 262144)
            throw new ArgumentException("request exceeds probe limits");
        var type = library.GetType($"Microsoft.SqlServer.TransactSql.ScriptDom.TSql{input.Version}Parser");
        if (type == null || !new[] { 80, 90, 100, 110, 120, 130, 140, 150, 160, 170, 180 }.Contains(input.Version))
            throw new ArgumentException("requested parser version is unavailable");
        var parser = (TSqlParser)(Activator.CreateInstance(type, new object[] { input.QuotedIdentifiers })
            ?? throw new ArgumentException("cannot create requested parser"));
        var fragment = parser.Parse(new StringReader(input.Sql), out var errors);
        result.Accepted = errors.Count == 0;
        result.Statements = fragment is TSqlScript script ? script.Batches.Sum(batch => batch.Statements.Count) : 0;
        result.Errors = errors.Take(16).Select(error => new NativeError
        {
            Number = error.Number, Line = error.Line, Column = error.Column,
            Message = error.Message.Length > 2048 ? error.Message[..2048] : error.Message
        }).ToArray();
        if (result.Accepted && input.IncludeShape && fragment is TSqlScript shaped)
            result.Shapes = shaped.Batches.SelectMany(batch => batch.Statements).Take(16).Select(statement =>
            {
                var shape = new NativeShape { Kind = statement.GetType().Name };
                if (statement is CreateTableStatement table)
                {
                    shape.Columns = table.Definition.ColumnDefinitions.Count;
                    shape.Indexes = table.Definition.Indexes.Count;
                    shape.ColumnNames = table.Definition.ColumnDefinitions.Take(16).Select(column => column.ColumnIdentifier.Value).ToArray();
                }
                if (statement is SelectStatement select && select.QueryExpression is QuerySpecification query)
                    shape.ExpressionKinds = query.SelectElements.Take(16).Select(element =>
                        element is SelectScalarExpression scalar ? scalar.Expression.GetType().Name : element.GetType().Name).ToArray();
                return shape;
            }).ToArray();
    }
    catch (Exception error) when (error is JsonException || error is ArgumentException || error is TargetInvocationException)
    {
        result.HarnessError = true;
        result.Error = error.Message.Length > 2048 ? error.Message[..2048] : error.Message;
    }
    Console.WriteLine(JsonSerializer.Serialize(result, json));
}

sealed class Request
{
    public int Index { get; set; }
    public string Sql { get; set; } = "";
    public int Version { get; set; } = 160;
    public bool QuotedIdentifiers { get; set; } = true;
    public bool IncludeShape { get; set; }
}
sealed class NativeError
{
    public int Number { get; set; }
    public int Line { get; set; }
    public int Column { get; set; }
    public string Message { get; set; } = "";
}
sealed class Response
{
    public int Index { get; set; }
    public bool Accepted { get; set; }
    public int ParserVersion { get; set; }
    public int Statements { get; set; }
    public NativeError[]? Errors { get; set; }
    public bool HarnessError { get; set; }
    public string? Error { get; set; }
    public NativeShape[]? Shapes { get; set; }
}
sealed class NativeShape
{
    public string Kind { get; set; } = "";
    public int? Columns { get; set; }
    public int? Indexes { get; set; }
    public string[]? ColumnNames { get; set; }
    public string[]? ExpressionKinds { get; set; }
}
