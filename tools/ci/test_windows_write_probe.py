#!/usr/bin/env python3
"""Reject mismatched/partial writable checkpoint evidence before mounted trials."""
from copy import deepcopy
import json
import unittest
from verify_windows_write_probe import validate_pair, validate_continuation, unique_object

COMMIT = "a" * 40
def reports(profile):
    basic = profile == "basic"
    size, inode, count = {"basic": (7001, 2, 1), "mutations": (89, 2, 2),
                          "delete": (37, 3, 1), "crash": (7001, 2, 1)}[profile]
    a = dict(schema=1 if basic else 2, phase="write" if basic else "ack" if profile == "crash" else "state",
             commit=COMMIT, source_process=17, observer_process=17, user_bytes=size, inode=inode,
             namespace_sequence=9, generation=18, image_bytes=1966080, image_cap=4194304,
             contexts=1 if profile == "crash" else 0, explicit_flush_observed=True,
             cpu_verified=True, dispatcher_stopped=profile != "crash", gpu=False)
    if not basic:
        a.update(profile=profile, file_count=count, creation_time=100, last_access_time=100,
                 last_write_time=100, change_time=100, attributes=32, reservation=8192,
                 data_digest="1" * 64, security_digest="2" * 64,
                 keep_digest=("3" if profile == "mutations" else "0") * 64)
    b = deepcopy(a);b.update(phase="verify", observer_process=18, contexts=0,
                             explicit_flush_observed=False, dispatcher_stopped=False)
    return a, b

class Evidence(unittest.TestCase):
    def test_complete_profiles(self):
        for profile in ("basic", "mutations", "delete", "crash"):
            with self.subTest(profile=profile):
                self.assertTrue(validate_pair(*reports(profile), profile, COMMIT)["complete"])

    def test_external_crash_actor_binding(self):
        # Historical schema-2 observations remain readable, but cannot be used
        # as evidence for the new live external-client campaign.
        legacy = reports("crash")
        self.assertTrue(validate_pair(*legacy, "crash", COMMIT)["complete"])
        with self.assertRaises(ValueError):
            validate_pair(*legacy, "crash", COMMIT, require_external=True)
        pair = reports("crash")
        for position, item in enumerate(pair):
            item.update(source_protocol="external-client", client_process=23,
                        host_self_io_rejected=position == 0,
                        client_flush_callback_observed=position == 0)
        self.assertTrue(validate_pair(*pair, "crash", COMMIT, require_external=True)["complete"])
        for position in (0, 1):
            for key in ("source_protocol", "client_process", "host_self_io_rejected",
                        "client_flush_callback_observed"):
                invalid = deepcopy(pair)
                del invalid[position][key]
                with self.subTest(position=position, missing=key), self.assertRaises(ValueError):
                    validate_pair(*invalid, "crash", COMMIT, require_external=True)
            for key, value in (("client_process", True), ("client_process", 0),
                               ("client_process", 17), ("client_process", 18),
                               ("client_process", 29), ("source_protocol", "self-client"),
                               ("host_self_io_rejected", position != 0),
                               ("host_self_io_rejected", int(position == 0)),
                               ("client_flush_callback_observed", position != 0)):
                invalid = deepcopy(pair)
                invalid[position][key] = value
                with self.subTest(position=position, key=key, value=value), self.assertRaises(ValueError):
                    validate_pair(*invalid, "crash", COMMIT, require_external=True)
        wrong = reports("mutations")
        for item in wrong:
            item["profile"] = "restart"
        for position, item in enumerate(wrong):
            item.update(source_protocol="external-client", client_process=23,
                        host_self_io_rejected=position == 0,
                        client_flush_callback_observed=position == 0)
        with self.assertRaises(ValueError):
            validate_pair(*wrong, "restart", COMMIT)

    def test_ordered_external_clients(self):
        for profile in ("basic", "mutations", "delete"):
            legacy = reports(profile)
            self.assertTrue(validate_pair(*legacy, profile, COMMIT)["complete"])
            with self.assertRaises(ValueError):
                validate_pair(*legacy, profile, COMMIT, require_external=True)
            pair = reports(profile)
            for position, item in enumerate(pair):
                item.update(source_protocol="external-client", client_process=23,
                            host_self_io_rejected=position == 0,
                            client_flush_callback_observed=position == 0)
            with self.subTest(profile=profile):
                self.assertTrue(validate_pair(*pair, profile, COMMIT, require_external=True)["complete"])
            for position in (0, 1):
                for key in ("source_protocol", "client_process", "host_self_io_rejected",
                            "client_flush_callback_observed"):
                    invalid = deepcopy(pair)
                    del invalid[position][key]
                    with self.subTest(profile=profile, position=position, missing=key), self.assertRaises(ValueError):
                        validate_pair(*invalid, profile, COMMIT, require_external=True)
                for key, value in (("client_process", True), ("client_process", 0),
                                   ("client_process", 17), ("client_process", 18),
                                   ("client_process", 29), ("source_protocol", "self-client"),
                                   ("host_self_io_rejected", position != 0),
                                   ("host_self_io_rejected", int(position == 0)),
                                   ("client_flush_callback_observed", position != 0)):
                    invalid = deepcopy(pair)
                    invalid[position][key] = value
                    with self.subTest(profile=profile, position=position, key=key, value=value), self.assertRaises(ValueError):
                        validate_pair(*invalid, profile, COMMIT, require_external=True)

    def test_missing_extra_and_mismatched_fields(self):
        for profile in ("basic", "mutations", "delete", "crash"):
            original = reports(profile)
            for position in (0, 1):
                for key in original[position]:
                    pair = deepcopy(original);del pair[position][key]
                    with self.subTest(profile=profile, position=position, missing=key):
                        with self.assertRaises(ValueError):validate_pair(*pair, profile, COMMIT)
                pair = deepcopy(original);pair[position]["unknown"] = 1
                with self.assertRaises(ValueError):validate_pair(*pair, profile, COMMIT)
            for key in ("commit", "source_process", "user_bytes", "inode", "namespace_sequence",
                        "generation", "image_bytes", "image_cap", "contexts", "cpu_verified",
                        "dispatcher_stopped", "explicit_flush_observed", "gpu"):
                pair = deepcopy(original)
                value = pair[1][key]
                pair[1][key] = not value if type(value) is bool else "b" * 40 if key == "commit" else value + 1
                with self.subTest(profile=profile, changed=key):
                    with self.assertRaises(ValueError):validate_pair(*pair, profile, COMMIT)

    def test_acknowledged_metadata_and_data_cannot_change(self):
        for profile in ("mutations", "delete", "crash"):
            for key in ("creation_time", "last_access_time", "last_write_time", "change_time",
                        "attributes", "reservation", "data_digest", "security_digest", "keep_digest", "file_count"):
                pair = reports(profile);value = pair[1][key]
                pair[1][key] = "4" * 64 if isinstance(value, str) else value + 4096 if key == "reservation" else value + 1
                with self.subTest(profile=profile, changed=key):
                    with self.assertRaises(ValueError):validate_pair(*pair, profile, COMMIT)

    def test_type_confusion_and_budget(self):
        for profile in ("basic", "mutations", "delete", "crash"):
            for key in ("schema", "source_process", "observer_process", "user_bytes", "inode",
                        "namespace_sequence", "generation", "image_bytes", "image_cap", "contexts"):
                pair = reports(profile);pair[1][key] = True
                with self.subTest(profile=profile, key=key):
                    with self.assertRaises(ValueError):validate_pair(*pair, profile, COMMIT)
            for key in ("cpu_verified", "gpu", "explicit_flush_observed", "dispatcher_stopped"):
                pair = reports(profile);pair[1][key] = int(pair[1][key])
                with self.assertRaises(ValueError):validate_pair(*pair, profile, COMMIT)
            pair = reports(profile);pair[0]["image_bytes"] = 4194305
            with self.assertRaises(ValueError):validate_pair(*pair, profile, COMMIT)
            pair = reports(profile);pair[0]["observer_process"] = 99
            with self.assertRaises(ValueError):validate_pair(*pair, profile, COMMIT)

    def test_restart_continuation(self):
        for profile in ("restart", "crash-restart"):
            a, b = reports("crash" if profile == "crash-restart" else "mutations")
            a.update(profile=profile, phase="ack" if profile == "crash-restart" else "state",
                     user_bytes=7001, inode=2, file_count=1, keep_digest="0" * 64,
                     contexts=1 if profile == "crash-restart" else 0,
                     dispatcher_stopped=profile == "restart")
            b = deepcopy(a)
            b.update(phase="verify", observer_process=18, contexts=0, explicit_flush_observed=False,
                     dispatcher_stopped=False, file_count=2, keep_digest="3" * 64,
                     namespace_sequence=a["namespace_sequence"]+5, generation=a["generation"]+10,
                     image_bytes=a["image_bytes"]+65536, last_access_time=a["last_access_time"]+1,
                     state_digest="4"*64, source_ack_digest="5"*64, resumer_process=19)
            continued = dict(schema=1, profile=profile, phase="continued", commit=COMMIT,
                             source_process=17, observer_process=19, source_ack_digest="5"*64,
                             continued_state_digest="4"*64, generation=b["generation"],
                             namespace_sequence=b["namespace_sequence"], image_bytes=b["image_bytes"])
            result = validate_continuation(a, continued, b, profile, COMMIT)
            self.assertTrue(result["complete"])
            if profile == "crash-restart":
                external_source, external_final = deepcopy(a), deepcopy(b)
                for position, item in enumerate((external_source, external_final)):
                    item.update(source_protocol="external-client", client_process=23,
                                host_self_io_rejected=position == 0,
                                client_flush_callback_observed=position == 0)
                self.assertTrue(validate_continuation(external_source, continued, external_final,
                    profile, COMMIT, require_external=True)["complete"])
                with self.assertRaises(ValueError):
                    validate_continuation(a, continued, b, profile, COMMIT, require_external=True)
                for alias in (17, 18, 19):
                    bad_source, bad_final = deepcopy(external_source), deepcopy(external_final)
                    bad_source["client_process"] = bad_final["client_process"] = alias
                    with self.subTest(client_alias=alias), self.assertRaises(ValueError):
                        validate_continuation(bad_source, continued, bad_final,
                                              profile, COMMIT, require_external=True)

            for key in ("image_bytes", "generation", "namespace_sequence"):
                self.assertEqual(result[key], b[key], "hosted budget must use the final continued state")
            for key in continued:
                bad = deepcopy(continued)
                value = bad[key]
                bad[key] = ("6"*64 if "digest" in key else "wrong") if isinstance(value, str) else value+1
                with self.subTest(profile=profile, changed=key):
                    with self.assertRaises(ValueError):validate_continuation(a, bad, b, profile, COMMIT)
                missing = deepcopy(continued);del missing[key]
                with self.assertRaises(ValueError):validate_continuation(a, missing, b, profile, COMMIT)
            for key in ("user_bytes", "creation_time", "attributes", "reservation", "data_digest",
                        "security_digest", "last_write_time", "change_time", "state_digest", "source_ack_digest"):
                bad = deepcopy(b);value = bad[key]
                bad[key] = "6"*64 if isinstance(value, str) else value+4096 if key=="reservation" else value+1
                with self.subTest(profile=profile, final=key):
                    with self.assertRaises(ValueError):validate_continuation(a, continued, bad, profile, COMMIT)
            bad = deepcopy(b);bad["generation"] = a["generation"]
            with self.assertRaises(ValueError):validate_continuation(a, continued, bad, profile, COMMIT)

    def test_wrong_profile_and_duplicate_json(self):
        for selected in ("basic", "mutations", "delete", "crash"):
            for actual in ("basic", "mutations", "delete", "crash"):
                if selected != actual:
                    with self.assertRaises(ValueError):validate_pair(*reports(actual), selected, COMMIT)
        with self.assertRaises(ValueError):
            json.loads('{"schema":1,"schema":2}', object_pairs_hook=unique_object)

if __name__ == "__main__":
    unittest.main()
