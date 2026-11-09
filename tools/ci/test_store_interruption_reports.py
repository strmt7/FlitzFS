#!/usr/bin/env python3
"""No files/children: complete fault-coverage proof and rejection tests."""
import copy
import unittest
from probe_store_interruptions import merge_reports,validate_partitions

def reports():
    coverage=[dict(policy="raw",phase="mutation",
                   events=[dict(op=op) for op in ("write-before","write-after","flush-before","flush-after")],
                   completed=dict(recovered=2,continued=3))]
    output=[]
    for partition in range(2):
        output.append(dict(schema=2,commit="a"*40,platform="nt",complete=True,
            partition=partition,partitions=2,planned_cases=4,cases=2,coverage=copy.deepcopy(coverage),
            scope="test",limits=dict(partition_seconds=180,process_seconds=15),elapsed_seconds=1,
            results=[dict(ordinal=i,policy="raw",phase="mutation",cut=i+1,
                          operation=coverage[0]["events"][i]["op"],forced_exit=1,
                          acknowledged=1,recovered=2,continued=3) for i in range(partition,4,2)]))
    return output

class CampaignReports(unittest.TestCase):
    def test_exact_union_and_order(self):
        merged=merge_reports(list(reversed(reports())))
        self.assertEqual(merged["cases"],4)
        self.assertEqual([r["ordinal"] for r in merged["results"]],list(range(4)))
        self.assertIsNone(merged["partition"])
        self.assertTrue(merged["complete"])

    def test_lost_duplicate_relabelled_or_unsafe_case_rejected(self):
        def reject(mutation):
            data=reports();mutation(data)
            with self.assertRaises(RuntimeError):
                merge_reports(data)
        reject(lambda r:r.pop())
        reject(lambda r:r[1].update(partition=0))
        reject(lambda r:r[1]["results"].pop())
        reject(lambda r:r[1]["results"][0].update(ordinal=0))
        reject(lambda r:r[1]["results"][0].update(cut=4))
        reject(lambda r:r[1]["results"][0].update(operation="ack"))
        reject(lambda r:r[1]["results"][0].update(forced_exit=0))
        reject(lambda r:r[1]["results"][0].update(recovered=0))
        reject(lambda r:r[1]["results"][0].update(continued=2))
        reject(lambda r:r[1].update(complete=False))
        reject(lambda r:r[1].update(commit="b"*40))
        reject(lambda r:r[1].update(platform="posix"))
        reject(lambda r:r[1].update(planned_cases=5))
        reject(lambda r:r[1]["limits"].update(process_seconds=99))
        reject(lambda r:r[1]["coverage"][0]["events"][0].update(op="wrong"))
        reject(lambda r:[x.update(planned_cases=1025) for x in r])

    def test_partition_bounds(self):
        for partition,parts in ((0,1),(0,2),(1,2)):
            validate_partitions(partition,parts)
        for partition,parts in ((-1,2),(2,2),(0,0),(0,3),(1,1)):
            with self.assertRaises(RuntimeError):
                validate_partitions(partition,parts)

if __name__=="__main__":
    unittest.main()
