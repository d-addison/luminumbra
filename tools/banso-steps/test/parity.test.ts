import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import {
  argvSnapshotsFixturePath,
  buildArgvSnapshots,
  type ArgvSnapshotFixture,
} from "../scripts/regen-fixtures.ts";

test("wrapper argv matches the committed snapshots", async () => {
  const committed = JSON.parse(
    await readFile(argvSnapshotsFixturePath, "utf8"),
  ) as ArgvSnapshotFixture;

  assert.deepEqual(buildArgvSnapshots(), committed);
});
