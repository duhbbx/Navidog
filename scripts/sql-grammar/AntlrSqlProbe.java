import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.Base64;
import org.antlr.v4.runtime.*;

/** Offline batch recognition only; never connects to a database or executes SQL. */
public final class AntlrSqlProbe {
    private static final class Errors extends BaseErrorListener {
        int count;
        String first = "";
        @Override public void syntaxError(Recognizer<?, ?> recognizer, Object token,
                int line, int column, String message, RecognitionException error) {
            if (count++ == 0) first = line + ":" + (column + 1) + " " + message;
        }
    }
    private static void configureMySql(Object recognizer) throws Exception {
        if (!recognizer.getClass().getSimpleName().startsWith("MySQL")) return;
        // Match the project's default MySQL lexical mode, not the upstream
        // Java base's ANSI_QUOTES default. Version predicates use MySQL 8.4.
        recognizer.getClass().getField("serverVersion").setInt(recognizer, 80400);
        ((java.util.Set<?>) recognizer.getClass().getField("sqlModes").get(recognizer)).clear();
    }
    public static void main(String[] args) throws Exception {
        if (args.length != 3) throw new IllegalArgumentException("lexer parser entryRule");
        final var lexerConstructor = Class.forName(args[0]).getConstructor(CharStream.class);
        final var parserConstructor = Class.forName(args[1]).getConstructor(TokenStream.class);
        final var rule = Class.forName(args[1]).getMethod(args[2]);
        try (var input = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8))) {
            String line;
            int index = 0;
            while ((line = input.readLine()) != null) {
                final var errors = new Errors();
                boolean eof = false;
                try {
                    final var sql = new String(Base64.getDecoder().decode(line), StandardCharsets.UTF_8);
                    if (sql.length() > 262144) throw new IllegalArgumentException("input limit exceeded");
                    final var lexer = (Lexer) lexerConstructor.newInstance(CharStreams.fromString(sql));
                    configureMySql(lexer);
                    lexer.removeErrorListeners();
                    lexer.addErrorListener(errors);
                    final var parser = (Parser) parserConstructor.newInstance(new CommonTokenStream(lexer));
                    configureMySql(parser);
                    parser.removeErrorListeners();
                    parser.addErrorListener(errors);
                    // PostgreSQL's upstream routine-body action reads contexts.
                    parser.setBuildParseTree(args[1].startsWith("PostgreSQL"));
                    rule.invoke(parser);
                    eof = parser.getCurrentToken().getType() == Token.EOF;
                } catch (Exception error) {
                    errors.count++;
                    if (errors.first.isEmpty()) errors.first = error.toString();
                }
                final var detail = Base64.getEncoder().encodeToString(errors.first.getBytes(StandardCharsets.UTF_8));
                System.out.println((index++) + "\t" + (errors.count == 0 && eof) + "\t" + detail);
            }
        }
    }
}
