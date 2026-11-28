package app.navidog.jdbc;

import java.sql.Connection;
import java.sql.SQLException;

final class DamengTls {
    static final int VERIFY_MODE = 1;
    private static final int MUTUAL_TLS = 1, SERVER_AUTHENTICATED_TLS = 5;

    static final class VerificationException extends SQLException {
        VerificationException(String message) {
            super(message);
        }
    }

    static void verify(Connection connection) throws VerificationException {
        // DM negotiates this mode from the server. The certificate parameters
        // alone do not require encryption on a server configured for plaintext.
        int mode;
        try {
            Class<?> vendor = Class.forName("dm.jdbc.driver.DmdbConnection", false,
                connection.getClass().getClassLoader());
            Object nativeConnection = vendor.isInstance(connection) ? connection : connection.unwrap(vendor);
            mode = vendor.getField("sslEncrypt").getInt(nativeConnection);
        } catch (ReflectiveOperationException | SQLException | RuntimeException error) {
            throw new VerificationException("此达梦 JDBC 版本无法报告 SSL 协商模式，请更换官方驱动版本");
        }
        if (mode != MUTUAL_TLS && mode != SERVER_AUTHENTICATED_TLS)
            throw new VerificationException("达梦服务器未协商带证书认证的 SSL 加密会话，请核对 ENABLE_ENCRYPT（需要 1 或 5）");
    }
}
