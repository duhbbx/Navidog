<?php
// Navidog JSON SQL bridge. Requires PHP 8+ and the matching official PDO driver.
// Configure NAVIDOG_TUNNEL_USER, NAVIDOG_TUNNEL_PASSWORD and NAVIDOG_DB_HOSTS
// in the web server environment. SQLite additionally needs NAVIDOG_SQLITE_ROOT.
declare(strict_types=1);
ini_set('display_errors', '0');
ini_set('log_errors', '1');
header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-store');
header('X-Content-Type-Options: nosniff');
function respond(array $value, int $status = 200): never {
    http_response_code($status);
    echo json_encode($value, JSON_INVALID_UTF8_SUBSTITUTE | JSON_THROW_ON_ERROR);
    exit;
}
$expectedUser = getenv('NAVIDOG_TUNNEL_USER') ?: '';
$expectedPassword = getenv('NAVIDOG_TUNNEL_PASSWORD') ?: '';
if ($expectedUser === '' || $expectedPassword === '') respond(['error'=>'Tunnel authentication is not configured'],503);
if (!hash_equals($expectedUser, $_SERVER['PHP_AUTH_USER'] ?? '') ||
    !hash_equals($expectedPassword, $_SERVER['PHP_AUTH_PW'] ?? '')) {
    header('WWW-Authenticate: Basic realm="Navidog"');
    respond(['error'=>'Tunnel authentication failed'],401);
}
if ($_SERVER['REQUEST_METHOD'] !== 'POST') respond(['error'=>'POST required'],405);
if ((int)($_SERVER['CONTENT_LENGTH'] ?? 0) > 4194304) respond(['error'=>'Request too large'],413);
try {
    $body = file_get_contents('php://input', false, null, 0, 4194305);
    if ($body === false || strlen($body) > 4194304) throw new RuntimeException('Request too large');
    $request = json_decode($body, true, 64, JSON_THROW_ON_ERROR);
    if (($request['protocol'] ?? '') !== 'navidog-1') throw new RuntimeException('Unsupported bridge protocol');
    $p = $request['connection'] ?? [];
    $kind = $p['kind'] ?? '';
    $host = $p['host'] ?? 'localhost';
    $database = $p['database'] ?? '';
    $port = (int)($p['port'] ?? 0);
    if (strpbrk($host . $database, ";\r\n\0") !== false) throw new RuntimeException('Invalid connection field');
    if ($kind === 'sqlite') {
        $root = realpath(getenv('NAVIDOG_SQLITE_ROOT') ?: '/nonexistent/navidog');
        $path = realpath($database);
        if (!$root || !$path || !is_file($path) || !str_starts_with($path, $root . DIRECTORY_SEPARATOR)) throw new RuntimeException('SQLite file is outside the configured directory');
        $dsn = 'sqlite:' . $path;
    } else {
        $allowed = explode(',', getenv('NAVIDOG_DB_HOSTS') ?: 'localhost,127.0.0.1,::1');
        if (!in_array($host, array_map('trim', $allowed), true)) throw new RuntimeException('Database host is not allowed by this bridge');
        if ($port < 1 || $port > 65535) throw new RuntimeException('Invalid database port');
        if ($kind === 'mysql' || $kind === 'oceanbase' || $kind === 'mariadb') $dsn = "mysql:host=$host;port=$port;dbname=$database;charset=utf8mb4";
        elseif ($kind === 'postgres') $dsn = "pgsql:host=$host;port=$port;dbname=$database;connect_timeout=5";
        else throw new RuntimeException('Unsupported database kind');
    }
    $options = [PDO::ATTR_ERRMODE=>PDO::ERRMODE_EXCEPTION,PDO::ATTR_DEFAULT_FETCH_MODE=>PDO::FETCH_NUM,PDO::ATTR_PERSISTENT=>false];
    if ($kind === 'mysql' || $kind === 'oceanbase' || $kind === 'mariadb') {
        $options[PDO::ATTR_EMULATE_PREPARES] = false;
        $options[PDO::MYSQL_ATTR_USE_BUFFERED_QUERY] = false;
        $options[PDO::MYSQL_ATTR_MULTI_STATEMENTS] = false;
    }
    $pdo = new PDO($dsn, $p['user'] ?? '', $p['password'] ?? '', $options);
    if (!empty($p['readOnly'])) {
        if ($kind === 'postgres') $pdo->exec('SET default_transaction_read_only = on');
        elseif ($kind === 'sqlite') $pdo->exec('PRAGMA query_only=ON');
        else $pdo->exec('SET SESSION TRANSACTION READ ONLY');
    }
    if ($kind === 'sqlite') $pdo->exec('PRAGMA foreign_keys=ON');
    if (($request['operation'] ?? '') === 'version') {
        $version = $pdo->getAttribute(PDO::ATTR_SERVER_VERSION);
        respond(['protocol'=>'navidog-1','version'=>(string)$version]);
    }
    $sql = $request['sql'] ?? '';
    if (!empty($request['base64'])) {
        $sql = base64_decode($sql, true);
        if ($sql === false) throw new RuntimeException('Invalid base64 SQL');
    }
    if (!is_string($sql) || trim($sql) === '') throw new RuntimeException('Empty SQL');
    $stmt = $pdo->prepare($sql);
    foreach (($request['parameters'] ?? []) as $index=>$parameter) {
        $type = $parameter['type'] ?? 'null'; $value = $parameter['value'] ?? null;
        if ($type === 'binary') { $value = base64_decode((string)$value, true); if ($value === false) throw new RuntimeException('Invalid binary parameter'); }
        $pdoType = match ($type) { 'null'=>PDO::PARAM_NULL, 'binary'=>PDO::PARAM_LOB, 'integer'=>PDO::PARAM_INT, 'bool'=>PDO::PARAM_BOOL, default=>PDO::PARAM_STR };
        $stmt->bindValue($index+1, $value, $pdoType);
    }
    $stmt->execute(); $columns = []; $rows = []; $cached = 0; $truncated = false; $cellTruncated = false;
    $maxRows = max(1,min(100000,(int)($request['maxRows'] ?? 1000)));
    $maxCell = max(1,min(1048576,(int)($request['maxCellBytes'] ?? 65536)));
    $maxBytes = max(128,min(16777216,(int)($request['maxBytes'] ?? 16777216)));
    $binaryColumns = [];
    for ($column=0; $column<$stmt->columnCount(); ++$column) {
        $meta = $stmt->getColumnMeta($column); $columns[] = $meta['name'] ?? (string)$column;
        $native = strtolower($meta['native_type'] ?? '');
        $binaryColumns[] = $native === 'bytea' || ($kind === 'sqlite' && $native === 'blob') || in_array('binary', $meta['flags'] ?? [], true);
    }
    while (($row = $stmt->fetch(PDO::FETCH_NUM)) !== false) {
        if (count($rows) >= $maxRows) { $truncated=true; break; }
        $encoded=[]; $size=64+count($row)*48;
        foreach ($row as $columnIndex=>$value) {
            if ($value === null) { $encoded[]=['type'=>'null']; continue; }
            if (is_resource($value)) $value = stream_get_contents($value, $maxCell+1);
            if (is_int($value)) { $encoded[]=['type'=>'integer','value'=>(string)$value]; $size+=32; continue; }
            if (is_float($value)) { $encoded[]=['type'=>'float','value'=>(string)$value]; $size+=32; continue; }
            $value=(string)$value;
            if (strlen($value)>$maxCell) { $value=substr($value,0,$maxCell); $cellTruncated=true; }
            $size+=strlen($value)*2; $encoded[]=['type'=>($binaryColumns[$columnIndex] || preg_match('//u', $value) !== 1 ? 'binary':'bytes'),'value'=>base64_encode($value)];
        }
        if ($cached+$size>$maxBytes) { $truncated=true; break; }
        $cached+=$size; $rows[]=$encoded;
    }
    $affected=count($columns) ? 0 : $stmt->rowCount(); $stmt->closeCursor();
    respond(['protocol'=>'navidog-1','columns'=>$columns,'rows'=>$rows,'affected'=>(string)$affected,'cachedBytes'=>$cached,'truncated'=>$truncated,'cellTruncated'=>$cellTruncated]);
} catch (Throwable $error) {
    // PDO messages may embed connection details; return SQLSTATE and message only.
    $message = $error->getMessage();
    if (isset($p['password']) && $p['password'] !== '') $message = str_replace($p['password'], '[redacted]', $message);
    respond(['protocol'=>'navidog-1','error'=>$message],400);
}
