# KV-Store Wire Protocol

All communication is over a single persistent TCP connection. Every request
and response is a newline-terminated line (`\n`). Fields within a line are
separated by `|` (pipe). The one exception is `LOGIN`, which returns two lines.

## Connection lifecycle

Every connection starts in the `AUTH` state and must complete either
`REGISTER` or `LOGIN` before any data command (`SET`/`GET`/`DEL`) is
accepted. Data commands sent before authentication are rejected.

### `REGISTER|<username>|<password>`
Creates a new user and authenticates this connection as that user.
After `REGISTER`, the connection is immediately authenticated — no `LOGIN`
step is needed before issuing data commands on the same socket.

**Response:** `REGISTER_OK|<user_id>\n`
**Rejected:** `AUTH_REJECT|EMPTY_CREDENTIALS\n`

### `LOGIN|<user_id>|<password>`
Authenticates this connection as an existing user.

**Response (two lines):** `AUTH_OK|<user_id>\n` followed by `Hi <username>\n`
**Rejected (connection closed after):** `AUTH_REJECT|INVALID_CREDENTIALS\n`

## Data commands (require an authenticated connection)

Keys are namespaced per authenticated user_id server-side -- two different
users can use the identical literal key without collision, and a user can
only ever read or write their own data.

### `SET|<key>|<value>`
**Response:** `OK\n`
**Rejected:** `REJECT|INVALID_INPUT|EMPTY_KEY\n` or
`REJECT|INVALID_INPUT|EMPTY_VALUE\n`

### `GET|<key>`
**Response:** `<value>\n`, or `NOT_FOUND\n` if the key doesn't exist.
**Rejected:** `REJECT|INVALID_INPUT|EMPTY_KEY\n`

### `DEL|<key>`
**Response:** `OK\n`, or `NOT_FOUND\n` if the key didn't exist.
**Rejected:** `REJECT|INVALID_INPUT|EMPTY_KEY\n`

## Errors

- Unrecognized command: `ERROR|INVALID_COMMAND\n`
- Request exceeds `MAX_REQUEST_SIZE` (64 KB) before a `\n` is found: server
  responds `REQUEST IS TOO LONG\n` (spaces, not pipes) and closes the connection.

## Notes

- Values may contain `|` characters; only `SET`'s first two fields (command,
  key) are split off -- the remainder of the line is taken as the value
  verbatim, so a value can safely contain further pipes.
- The server accepts arbitrary bytes in keys/values except the client
  cannot include a literal `\n` inside a field (it always terminates the
  request/response line). There is no other reserved byte in the wire
  protocol itself.