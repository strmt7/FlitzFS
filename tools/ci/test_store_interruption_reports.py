#!/usr/bin/env python3
"""No files/children: complete fault-coverage proof and rejection tests."""
import copy
import unittest
from probe_store_interruptions import (merge_reports,validate_partitions,BASIC_PHASES,BASIC_SCOPE,
    campaign_limits,validate_basic_report)

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


def basic_reports():
    coverage=[]
    for policy in ("raw","auto"):
        for phase in BASIC_PHASES:
            events=[]
            def emit(op,offset=0,size=0,generation=0,digest=None):
                event=dict(event=len(events)+1,op=op,offset=offset,bytes=size,generation=generation)
                if digest is not None:
                    event["state_digest"]=digest
                events.append(event)
            emit("ack",generation=4,digest="a"*64)
            for offset,size in ((131072,128),(0,4096),(65536,4096)):
                emit("write-before",offset,size);emit("write-after",offset,size)
                emit("flush-before");emit("flush-after")
            emit("ack",generation=6,digest="b"*64)
            coverage.append(dict(policy=policy,phase=phase,events=events,
                completed=dict(acknowledged=6,forced_exit=None,recovered=6,continued=8,
                               state_digest="b"*64,continued_digest="c"*64)))
    results=[];ordinal=0
    for item in coverage:
        for cut,event in enumerate(item["events"],1):
            generation=6 if cut==len(item["events"]) else 5 if cut>=8 else 4
            results.append(dict(ordinal=ordinal,policy=item["policy"],phase=item["phase"],cut=cut,
                operation=event["op"],forced_exit=1,acknowledged=6 if cut==len(item["events"]) else 4,
                recovered=generation,continued=generation+2,state_digest=("a" if generation==4 else "b")*64,
                continued_digest="c"*64))
            ordinal+=1
    return [dict(schema=3,campaign="basic",commit="d"*40,platform="nt",complete=True,
                 partition=p,partitions=2,planned_cases=len(results),cases=len(results[p::2]),
                 coverage=copy.deepcopy(coverage),scope=BASIC_SCOPE,limits=campaign_limits(2),
                 elapsed_seconds=1,results=copy.deepcopy(results[p::2])) for p in range(2)]

class BasicCampaignReports(unittest.TestCase):
    def test_complete_digest_bound_union(self):
        data=basic_reports()
        for report in data:
            validate_basic_report(report)
        combined=merge_reports(list(reversed(data)))
        self.assertEqual(combined["schema"],3)
        self.assertEqual(combined["campaign"],"basic")
        self.assertEqual(combined["cases"],84)
        self.assertEqual([r["ordinal"] for r in combined["results"]],list(range(84)))

    def test_basic_incomplete_wrong_scope_or_digest_rejected(self):
        def reject(mutation):
            data=basic_reports();mutation(data)
            with self.assertRaises(RuntimeError):
                merge_reports(data)
        reject(lambda r:r[0]["results"][0].update(state_digest="b"*64))
        reject(lambda r:r[0]["results"][0].update(continued_digest="a"*64))
        reject(lambda r:r[0]["results"][0].update(continued_digest="bad"))
        reject(lambda r:r[0]["results"][0].update(acknowledged=6))
        reject(lambda r:r[0]["results"][0].update(recovered=True))
        reject(lambda r:r[0]["results"][0].update(cut=1.0))
        reject(lambda r:r[0]["results"][0].update(forced_exit=True))
        reject(lambda r:r[0]["results"].pop())
        reject(lambda r:r[0]["results"].append(copy.deepcopy(r[0]["results"][0])))
        reject(lambda r:r[0].update(complete="yes"))
        reject(lambda r:[x.update(campaign="legacy") for x in r])
        reject(lambda r:[x.update(scope="unbound") for x in r])
        reject(lambda r:[x.update(schema=2) for x in r])
        reject(lambda r:[x.update(platform="posix") for x in r])
        reject(lambda r:[x["limits"].update(cases=2048) for x in r])
        reject(lambda r:[x["limits"].update(event_bytes=1024) for x in r])
        reject(lambda r:[x["coverage"].pop() for x in r])
        reject(lambda r:[x["coverage"][0].update(phase="basic-update") for x in r])
        reject(lambda r:[x["coverage"][0]["completed"].update(state_digest="a"*64) for x in r])
        reject(lambda r:[x["coverage"][0]["completed"].update(continued=6) for x in r])
        reject(lambda r:[x["coverage"][0]["events"][0].update(state_digest="b"*64) for x in r])
        reject(lambda r:[x["coverage"][0]["events"][0].update(source_digest="e"*64) for x in r])
        reject(lambda r:[x["coverage"][0]["events"][1].update(bytes=65537) for x in r])
        reject(lambda r:[x["coverage"][0]["events"][2].update(offset=0) for x in r])
        reject(lambda r:[x["coverage"][0]["events"][1].update(event=True) for x in r])
        reject(lambda r:[x["coverage"][0]["events"][1].update(state_digest="e"*64) for x in r])
        # Remove a whole barrier pair while relabelling the remaining trace.
        # A matching inventory alone must not make two barriers a valid commit.
        def remove_barrier(data):
            for report in data:
                del report["coverage"][0]["events"][3:5]
                for i,event in enumerate(report["coverage"][0]["events"],1):
                    event["event"]=i
        reject(remove_barrier)

if __name__=="__main__":
    unittest.main()
