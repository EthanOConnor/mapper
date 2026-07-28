# Connected-editing cross-language fixtures

These are the authoritative bootstrap, transaction, chain, and projection
vectors shared with Mapper.

Mapper's `map_hub_protocol_fixture_t` generated the pair from fixed UUID source
data through `XMLFileExporter`, reimported the OMAP, rebuilt the entity index,
and verified a byte-for-byte round trip. The map contains:

- one top-level map part;
- three top-level symbols, including a point symbol whose element contains a
  nested line symbol;
- three top-level point, path, and text objects.

Nested component symbol and object UUIDs remain in the outer symbol fragment
and are intentionally absent from the top-level entity index.

Mapper's `MapHubEditTransaction::fromUndoStep` also produced
`transaction-combined-1.json` from one real `CombinedUndoStep`: replace the
path, delete the text, and create a point. `transaction-undo-2.json` is the
ordinary new transaction that reverses those entity changes. Projection files
are Map Hub's exact entity indexes after each transaction.

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `bootstrap.omap` | 3047 | `3f0b82424ea105435374bd96e51f81c1d0e0983b8c3139972fe2bb6126472c96` |
| `bootstrap-index.json` | 1258 | `81ed307448bbc83caa76142f63556f6ba1b7f0d7e88ac458f2f75693b1b67ea4` |
| `transaction-combined-1.json` | 1395 | `caadb782755ad5c6413728904d976204ccfc79703fb66ba585a70723280b7415` |
| `projection-1.json` | 1382 | `4e0fad6759c17313d83d9233f2ce2c28b6c125999c9e365fcc4703de3633b210` |
| `transaction-undo-2.json` | 1453 | `7b2bd3179e5d7ce6ebdf516740c8c00d1e8d7cbc7d1e9606cfcae7387a962e65` |
| `projection-2.json` | 1382 | `47ee86136932d22b8da759d27b36a8bfad14c97e2cada465210a6e2da3c0d531` |

The transaction payload digests equal the transaction-file SHA-256 values.
With the zero genesis hash, the assigned stream chain hashes are:

- sequence 1: `e6ff48d917d96081c9bd2929da23cf54abba1a223077516c8dba33a4d5ae4c2b`;
- sequence 2: `212cc13d6411565a379a030aca679266e96eab8bd839c9df42de1b30bd23f38a`.

Do not hand-edit the Mapper-produced files. Regenerate them through Mapper,
then update the server projection and hash assertions in
`lifecycle/tests/test_connected_editing.py`.
