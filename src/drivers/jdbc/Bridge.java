package app.navidog.jdbc;

import java.io.*;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.sql.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;

/** Binary JDBC bridge. DataInputStream matches Qt's big-endian QDataStream 5.15. */
public final class Bridge {
    private static final int MAX_FRAME = 64 * 1024 * 1024;
    private static final int MAX_COLUMNS = 4096;
    private static final int CANCEL = 0, OPEN = 1, CLOSE = 2, QUERY = 3, AUTO_COMMIT = 4;
    private static final int COMMIT = 5, ROLLBACK = 6, TABLES = 7, METADATA = 8;
    private static final int OK = 0, ERROR = 1, DISCONNECTED = 2, CANCELLED = 3;
    private static final DataOutputStream output = new DataOutputStream(
        new BufferedOutputStream(new FileOutputStream(FileDescriptor.out)));
    private static final Map<Integer, Session> sessions = new ConcurrentHashMap<>();
    private static final Map<Integer, Request> requests = new ConcurrentHashMap<>();
    private static final AtomicInteger nextSession = new AtomicInteger(1);
    private static final ExecutorService workers = pool(8, 32);
    private static final ExecutorService cancellations = pool(2, 32);

    private record DriverKey(Path jar, String name) {}
    private record LoadedDriver(Driver driver, URLClassLoader loader) {}
    private record Session(Connection connection, ClassLoader loader) {}
    private static final Map<DriverKey, LoadedDriver> drivers = new HashMap<>();

    private static synchronized LoadedDriver loadDriver(DriverKey key) throws Exception {
        LoadedDriver loaded = drivers.get(key);
        if (loaded != null) return loaded;
        if (drivers.size() >= 16) throw new IOException("已达到 JDBC 驱动加载数量上限");
        URLClassLoader loader = new URLClassLoader(
            new URL[]{key.jar().toUri().toURL()}, Bridge.class.getClassLoader());
        try {
            if ("org.h2.Driver".equals(key.name())) System.setProperty("h2.bindAddress", "127.0.0.1");
            Driver driver = (Driver) Class.forName(key.name(), true, loader).getDeclaredConstructor().newInstance();
            loaded = new LoadedDriver(driver, loader);
            drivers.put(key, loaded);
            return loaded;
        } catch (Exception | LinkageError error) {
            loader.close();
            throw error;
        }
    }

    private static final class Request {
        final int id;
        final int session;
        volatile boolean cancelled;
        volatile Statement statement;

        Request(int id, int session) {
            this.id = id;
            this.session = session;
        }

        void checkCancelled() throws InterruptedIOException {
            if (cancelled) throw new InterruptedIOException();
        }

        void cancel() {
            cancelled = true;
            Statement current = statement;
            if (current != null) {
                try {
                    cancellations.execute(() -> {
                        try { current.cancel(); } catch (SQLException ignored) {}
                    });
                } catch (RejectedExecutionException ignored) {
                    // The client still has a deadline if the driver does not cancel.
                }
            }
        }
    }

    private static ExecutorService pool(int threads, int queue) {
        return new ThreadPoolExecutor(threads, threads, 0L, TimeUnit.SECONDS,
            new ArrayBlockingQueue<>(queue), task -> {
                Thread thread = new Thread(task);
                thread.setDaemon(true);
                return thread;
            });
    }

    public static void main(String[] args) throws IOException {
        // A vendor driver writing to System.out must never corrupt protocol frames.
        System.setOut(System.err);
        try (DataInputStream in = new DataInputStream(new BufferedInputStream(System.in))) {
            while (true) {
                int size;
                try { size = in.readInt(); } catch (EOFException end) { break; }
                if (size < 9 || size > MAX_FRAME) break;
                byte[] frame = in.readNBytes(size);
                if (frame.length != size) break;
                try (DataInputStream data = new DataInputStream(new ByteArrayInputStream(frame))) {
                    int id = data.readInt();
                    int operation = data.readUnsignedByte();
                    int session = data.readInt();
                    if (operation == CANCEL) {
                        Request request = requests.get(id);
                        if (request != null && request.session == session) request.cancel();
                    } else {
                        Request request = new Request(id, session);
                        requests.put(id, request);
                        try {
                            workers.execute(() -> handle(request, operation, data));
                        } catch (RejectedExecutionException busy) {
                            requests.remove(id);
                            respond(id, ERROR, errorText("JDBC 操作队列已满"));
                        }
                    }
                }
            }
        } finally {
            workers.shutdownNow();
            cancellations.shutdownNow();
        }
    }

    private static void handle(Request request, int operation, DataInputStream data) {
        int status = OK;
        Bytes response = new Bytes();
        try {
            request.checkCancelled();
            if (operation == OPEN) {
                open(data, response);
            } else {
                Session session = sessions.get(request.session);
                if (session == null) throw new SQLException("JDBC 会话不存在", "08003");
                synchronized (session) {
                    request.checkCancelled();
                    Connection connection = session.connection();
                    Thread thread = Thread.currentThread();
                    ClassLoader previous = thread.getContextClassLoader();
                    try {
                        thread.setContextClassLoader(session.loader());
                        switch (operation) {
                            case CLOSE -> {
                                sessions.remove(request.session);
                                try (connection) {
                                    if (!connection.getAutoCommit()) connection.rollback();
                                }
                            }
                            case QUERY -> query(request, connection, data, response);
                            case AUTO_COMMIT -> connection.setAutoCommit(data.readBoolean());
                            case COMMIT -> connection.commit();
                            case ROLLBACK -> connection.rollback();
                            case TABLES -> tables(connection, response);
                            case METADATA -> metadata(request, connection, data, response);
                            default -> throw new IOException("未知 JDBC 操作");
                        }
                    } finally {
                        thread.setContextClassLoader(previous);
                    }
                }
            }
            if (request.cancelled) status = CANCELLED;
        } catch (Exception | LinkageError error) {
            if (request.cancelled) {
                status = CANCELLED;
            } else {
                boolean disconnected = error instanceof SQLException sql && sql.getSQLState() != null
                    && sql.getSQLState().startsWith("08");
                status = disconnected ? DISCONNECTED : ERROR;
                try {
                    // Connection errors can quote driver properties. Keep credentials out of diagnostics.
                    response = errorText(operation == OPEN && !(error instanceof DamengTls.VerificationException)
                        ? "无法打开 JDBC 连接（" + error.getClass().getSimpleName() + sqlCode(error) + "）"
                        : error.getMessage() == null ? error.getClass().getSimpleName() : error.getMessage());
                } catch (IOException impossible) {
                    return;
                }
            }
        } finally {
            requests.remove(request.id);
        }
        try {
            respond(request.id, status, status == CANCELLED ? new Bytes() : response);
        } catch (IOException lostParent) {
            System.exit(1);
        }
    }

    private static String sqlCode(Throwable error) {
        if (error instanceof SQLException sql) return "，代码 " + sql.getErrorCode();
        return "";
    }

    private static void open(DataInputStream in, Bytes response) throws Exception {
        Path jar = Path.of(readText(in)).toRealPath();
        String driverClass = readText(in), url = readText(in);
        Properties properties = new Properties();
        int count = in.readInt();
        if (count < 0 || count > 256) throw new IOException("JDBC 连接属性数量无效");
        for (int i = 0; i < count; ++i) properties.setProperty(readText(in), readText(in));
        int verification = in.readUnsignedByte();
        if (verification != 0 && verification != DamengTls.VERIFY_MODE)
            throw new IOException("未知 JDBC 传输校验模式");
        LoadedDriver loaded;
        try {
            loaded = loadDriver(new DriverKey(jar, driverClass));
        } catch (ClassNotFoundException error) {
            // openGauss publishes both the standalone and PostgreSQL-namespaced JARs.
            String prefix = "jdbc:opengauss:";
            if (!driverClass.equals("org.opengauss.Driver") || !url.startsWith(prefix)) throw error;
            loaded = loadDriver(new DriverKey(jar, "org.postgresql.Driver"));
            url = "jdbc:postgresql:" + url.substring(prefix.length());
        }
        Connection connection = null;
        try {
            Thread thread = Thread.currentThread();
            ClassLoader previous = thread.getContextClassLoader();
            try {
                thread.setContextClassLoader(loaded.loader());
                connection = loaded.driver().connect(url, properties);
            } finally {
                thread.setContextClassLoader(previous);
            }
            if (connection == null) throw new SQLException("JDBC 驱动不支持连接地址");
            if (verification == DamengTls.VERIFY_MODE) DamengTls.verify(connection);
            if (!connection.getAutoCommit()) connection.setAutoCommit(true);
            String version = connection.getMetaData().getDatabaseProductVersion();
            int id = nextSession.getAndIncrement();
            response.data.writeInt(id);
            writeText(response.data, version);
            sessions.put(id, new Session(connection, loaded.loader()));
        } catch (Exception | LinkageError error) {
            if (connection != null) try { connection.close(); } catch (SQLException ignored) {}
            throw error;
        }
    }

    private static String exactPattern(DatabaseMetaData metadata, String name) throws SQLException {
        if (name.isEmpty()) return null;
        String escape = metadata.getSearchStringEscape();
        if (escape == null || escape.isEmpty()) {
            if (name.contains("%") || name.contains("_"))
                throw new SQLException("驱动未提供元数据匹配转义");
            return name;
        }
        return name.replace(escape, escape + escape).replace("%", escape + "%").replace("_", escape + "_");
    }

    private static void metadata(Request request, Connection connection, DataInputStream input, Bytes response)
        throws IOException, SQLException {
        int kind = input.readUnsignedByte();
        String schema = readText(input), object = readText(input);
        int maxRows = input.readInt(), maxBytes = input.readInt(), maxCell = input.readInt();
        if (maxRows < 1 || maxRows > 100000 || maxBytes < 128 || maxBytes > 48 * 1024 * 1024 ||
            maxCell < 1 || maxCell > 1048576)
            throw new IOException("JDBC 元数据结果限制无效");
        var metadata = connection.getMetaData();
        var schemaPattern = exactPattern(metadata, schema);
        var objectPattern = exactPattern(metadata, object);
        // null catalog selects the current database on engines without catalogs.
        try (ResultSet rows = switch (kind) {
            case 0 -> metadata.getSchemas();
            case 1 -> metadata.getTables(null, schemaPattern, objectPattern, new String[]{"TABLE", "BASE TABLE", "VIEW", "MATERIALIZED VIEW"});
            case 2 -> metadata.getColumns(null, schemaPattern, objectPattern, null);
            case 3 -> metadata.getPrimaryKeys(null, schema, object);
            case 4 -> metadata.getFunctions(null, schemaPattern, objectPattern);
            case 5 -> metadata.getProcedures(null, schemaPattern, objectPattern);
            default -> throw new SQLException("未知 JDBC 元数据请求");
        }) {
            request.statement = rows.getStatement();
            response.data.writeInt(1);
            collect(request, rows, maxRows, maxBytes, maxCell).bytes().writeTo(response);
        } finally {
            request.statement = null;
        }
    }

    private static void tables(Connection connection, Bytes response) throws SQLException, IOException {
        List<String> names = new ArrayList<>();
        try (ResultSet rows = connection.getMetaData().getTables(
            connection.getCatalog(), connection.getSchema(), "%", new String[]{"TABLE", "VIEW"})) {
            while (names.size() < 10000 && rows.next())
                names.add(cap(rows.getString("TABLE_NAME"), 512));
        }
        response.data.writeInt(names.size());
        for (String name : names) writeText(response.data, name);
    }

    private static void query(Request request, Connection connection, DataInputStream in,
                              Bytes response) throws SQLException, IOException {
        String sql = readText(in);
        int maxRows = in.readInt(), maxBytes = in.readInt(), maxCell = in.readInt();
        int timeout = in.readInt(), parameterCount = in.readInt();
        if (maxRows < 1 || maxRows > 100000 || maxBytes < 128 || maxBytes > 48 * 1024 * 1024 ||
            maxCell < 1 || maxCell > 1048576 || parameterCount < 0 || parameterCount > 65535)
            throw new IOException("JDBC 结果限制或参数数量无效");
        try (Statement statement = parameterCount == 0 ? connection.createStatement() : connection.prepareStatement(sql)) {
            request.statement = statement;
            if (timeout > 0) statement.setQueryTimeout(timeout);
            statement.setFetchSize(128);
            boolean hasRows;
            if (statement instanceof PreparedStatement prepared) {
                for (int index = 1; index <= parameterCount; ++index) bind(prepared, in, index);
                request.checkCancelled();
                hasRows = prepared.execute();
            } else {
                request.checkCancelled();
                hasRows = statement.execute(sql);
            }
            List<Bytes> results = new ArrayList<>();
            int remaining = maxBytes;
            while (results.size() < 64) {
                request.checkCancelled();
                Part result;
                if (hasRows) {
                    try (ResultSet rows = statement.getResultSet()) {
                        result = collect(request, rows, maxRows, remaining, maxCell);
                    }
                } else {
                    long affected = statement.getUpdateCount();
                    if (affected < 0 && !results.isEmpty()) break;
                    result = updateCount(Math.max(0, affected));
                }
                remaining -= result.cachedBytes();
                results.add(result.bytes());
                if (!hasRows && statement.getUpdateCount() == -1) break;
                try {
                    hasRows = statement.getMoreResults(Statement.CLOSE_CURRENT_RESULT);
                } catch (SQLFeatureNotSupportedException unsupported) {
                    break;
                }
            }
            response.data.writeInt(results.size());
            for (Bytes result : results) result.writeTo(response);
        } finally {
            request.statement = null;
        }
    }

    private static void bind(PreparedStatement statement, DataInputStream in, int index)
        throws SQLException, IOException {
        switch (in.readUnsignedByte()) {
            case 0 -> statement.setNull(index, Types.NULL);
            case 1 -> statement.setString(index, readText(in));
            case 2 -> statement.setBytes(index, readBytes(in));
            case 3 -> statement.setBoolean(index, in.readBoolean());
            case 4 -> statement.setLong(index, in.readLong());
            case 5 -> statement.setDouble(index, in.readDouble());
            default -> throw new IOException("无效 JDBC 参数类型");
        }
    }

    private record Part(Bytes bytes, int cachedBytes) {}

    private static Part updateCount(long affected) throws IOException {
        Bytes result = new Bytes();
        result.data.writeInt(0); // Columns
        result.data.writeInt(0); // Rows
        finish(result, affected, 0, false, false);
        return new Part(result, 0);
    }

    private static Part collect(Request request, ResultSet rows, int maxRows, int maxBytes, int maxCell)
        throws SQLException, IOException {
        ResultSetMetaData metadata = rows.getMetaData();
        int columns = metadata.getColumnCount();
        if (columns < 1 || columns > MAX_COLUMNS) throw new SQLException("JDBC 结果列数超出限制");
        int[] types = new int[columns];
        Bytes part = new Bytes();
        part.data.writeInt(columns);
        int cached = 0;
        for (int column = 1; column <= columns; ++column) {
            types[column - 1] = metadata.getColumnType(column);
            String name = cap(metadata.getColumnLabel(column), 512);
            String type = cap(metadata.getColumnTypeName(column), 128);
            writeText(part.data, name);
            writeText(part.data, type);
            cached += 48 + 2 * (name.length() + type.length());
        }
        // Column metadata participates in the same budget across all result sets.
        if (cached > maxBytes) {
            Bytes empty = new Bytes();
            empty.data.writeInt(0);
            empty.data.writeInt(0);
            finish(empty, 0, 0, true, false);
            return new Part(empty, 0);
        }
        Bytes body = new Bytes();
        int count = 0;
        boolean truncated = false, cellTruncated = false;
        while (rows.next()) {
            request.checkCancelled();
            if (count >= maxRows || cached + 64 + columns * 48 > maxBytes) {
                truncated = true;
                break;
            }
            Bytes row = new Bytes();
            int logicalSize = 64 + columns * 48;
            boolean clipped = false;
            for (int column = 1; column <= columns; ++column) {
                Cell cell = readCell(rows, column, types[column - 1], maxCell);
                logicalSize += cell.size();
                if (cached + logicalSize > maxBytes) break;
                cell.write(row.data);
                clipped |= cell.clipped();
            }
            if (cached + logicalSize > maxBytes) {
                truncated = true;
                break;
            }
            row.writeTo(body);
            cached += logicalSize;
            cellTruncated |= clipped;
            ++count;
        }
        part.data.writeInt(count);
        body.writeTo(part);
        finish(part, 0, cached, truncated, cellTruncated);
        return new Part(part, cached);
    }

    private record Cell(String text, byte[] binary, boolean clipped) {
        int size() { return binary != null ? binary.length : text == null ? 0 : text.length() * 2; }

        void write(DataOutputStream out) throws IOException {
            if (binary != null) {
                out.writeByte(2);
                out.writeInt(binary.length);
                out.write(binary);
            } else if (text != null) {
                out.writeByte(1);
                writeText(out, text);
            } else {
                out.writeByte(0);
            }
        }
    }

    private static Cell readCell(ResultSet rows, int column, int type, int limit)
        throws SQLException, IOException {
        if (type == Types.BLOB || type == Types.BINARY || type == Types.VARBINARY ||
            type == Types.LONGVARBINARY) {
            try (InputStream input = rows.getBinaryStream(column)) {
                if (input == null) return new Cell(null, null, false);
                byte[] bytes = input.readNBytes(limit + 1);
                boolean clipped = bytes.length > limit;
                return new Cell(null, clipped ? Arrays.copyOf(bytes, limit) : bytes, clipped);
            }
        }
        int characters = Math.max(1, limit / 2);
        if (type == Types.CLOB || type == Types.NCLOB || type == Types.LONGVARCHAR ||
            type == Types.LONGNVARCHAR || type == Types.SQLXML ||
            type == Types.VARCHAR || type == Types.NVARCHAR || type == Types.CHAR || type == Types.NCHAR) {
            try (Reader reader = rows.getCharacterStream(column)) {
                if (reader == null) return new Cell(null, null, false);
                StringBuilder text = new StringBuilder(Math.min(4096, characters));
                char[] buffer = new char[Math.min(4096, characters + 1)];
                while (text.length() <= characters) {
                    int read = reader.read(buffer, 0, Math.min(buffer.length, characters + 1 - text.length()));
                    if (read < 0) break;
                    text.append(buffer, 0, read);
                }
                return boundedText(text.toString(), characters);
            }
        }
        // getString preserves numeric precision and avoids a JVM-local timezone conversion.
        return boundedText(rows.getString(column), characters);
    }

    private static Cell boundedText(String value, int limit) {
        if (value == null || value.length() <= limit) return new Cell(value, null, false);
        int end = limit - 1;
        if (end > 0 && Character.isHighSurrogate(value.charAt(end - 1))) --end;
        return new Cell(value.substring(0, end) + "…", null, true);
    }

    private static void finish(Bytes buffer, long affected, long cached, boolean truncated,
                               boolean cellTruncated) throws IOException {
        buffer.data.writeLong(affected);
        buffer.data.writeLong(cached);
        buffer.data.writeBoolean(truncated);
        buffer.data.writeBoolean(cellTruncated);
    }

    private static String cap(String text, int max) {
        return text == null ? "" : text.length() > max ? text.substring(0, max) : text;
    }

    private static String readText(DataInputStream in) throws IOException {
        return new String(readBytes(in), StandardCharsets.UTF_16BE);
    }

    private static byte[] readBytes(DataInputStream in) throws IOException {
        int length = in.readInt();
        if (length == -1) return new byte[0]; // Qt null QByteArray/QString
        if (length < 0 || length > in.available()) throw new IOException("JDBC 请求帧不完整");
        return in.readNBytes(length);
    }

    private static void writeText(DataOutputStream out, String text) throws IOException {
        byte[] bytes = (text == null ? "" : text).getBytes(StandardCharsets.UTF_16BE);
        out.writeInt(bytes.length);
        out.write(bytes);
    }

    private static Bytes errorText(String text) throws IOException {
        Bytes response = new Bytes();
        writeText(response.data, cap(text, 4096));
        return response;
    }

    private static synchronized void respond(int id, int status, Bytes response) throws IOException {
        if (response.size() + 5 > MAX_FRAME) {
            status = ERROR;
            response = errorText("JDBC 返回内容超过大小上限");
        }
        output.writeInt(response.size() + 5);
        output.writeInt(id);
        output.writeByte(status);
        response.writeTo(output);
        output.flush();
    }

    private static final class Bytes extends ByteArrayOutputStream {
        final DataOutputStream data = new DataOutputStream(this);
    }
}
