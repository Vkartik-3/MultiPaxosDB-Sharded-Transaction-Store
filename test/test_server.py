import asyncio
import logging

import grpc
import tpc_pb2
import tpc_pb2_grpc
from google.protobuf.empty_pb2 import Empty


async def disconnected_tpc():
    channel1 = grpc.aio.insecure_channel("localhost:50001")
    channel2 = grpc.aio.insecure_channel("localhost:50002")
    channel3 = grpc.aio.insecure_channel("localhost:50003")
    channel4 = grpc.aio.insecure_channel("localhost:50004")
    channel5 = grpc.aio.insecure_channel("localhost:50005")
    channel6 = grpc.aio.insecure_channel("localhost:50006")
    # channel7 = grpc.aio.insecure_channel("localhost:50007")
    # channel8 = grpc.aio.insecure_channel("localhost:50008")
    # channel9 = grpc.aio.insecure_channel("localhost:50009")

    stub1 = tpc_pb2_grpc.TpcServerStub(channel1)
    stub2 = tpc_pb2_grpc.TpcServerStub(channel2)
    stub3 = tpc_pb2_grpc.TpcServerStub(channel3)
    stub4 = tpc_pb2_grpc.TpcServerStub(channel4)
    stub5 = tpc_pb2_grpc.TpcServerStub(channel5)
    stub6 = tpc_pb2_grpc.TpcServerStub(channel6)
    # stub7 = tpc_pb2_grpc.TpcServerStub(channel7)
    # stub8 = tpc_pb2_grpc.TpcServerStub(channel8)
    # stub9 = tpc_pb2_grpc.TpcServerStub(channel9)

    # Disconnect some servers
    response1 = stub1.Disconnect(
        tpc_pb2.DisconnectReq(servers=["S3", "S6"])
    )
    response2 = stub2.Disconnect(
        tpc_pb2.DisconnectReq(servers=["S3", "S6"])
    )
    response3 = stub3.Disconnect(
        tpc_pb2.DisconnectReq(servers=["S3", "S6"])
    )
    response4 = stub4.Disconnect(
        tpc_pb2.DisconnectReq(servers=["S3", "S6"])
    )
    response5 = stub5.Disconnect(
        tpc_pb2.DisconnectReq(servers=["S3", "S6"])
    )
    response6 = stub6.Disconnect(
        tpc_pb2.DisconnectReq(servers=["S3", "S6"])
    )
    await asyncio.gather(response1, response2, response3, response4, response5, response6)

    # cross shard transaction
    tid = 1
    response1 = stub1.TpcPrepare(
        tpc_pb2.TransferReq(
            txn = tpc_pb2.Transaction(sender=2, receiver=1002, amount=3),
            tid = tid
        )
    )

    response4 = stub4.TpcPrepare(
        tpc_pb2.TransferReq(
            txn = tpc_pb2.Transaction(sender=2, receiver=1002, amount=3),
            tid = tid
        )
    )

    await asyncio.gather(response1, response4)
    print("Response1: ", str(response1))
    print("Response4: ", str(response4))


    response1 = stub1.TpcAbort(tpc_pb2.TpcTid(tid = tid))
    response2 = stub2.TpcAbort(tpc_pb2.TpcTid(tid = tid))
    response4 = stub4.TpcAbort(tpc_pb2.TpcTid(tid = tid))
    response5 = stub5.TpcAbort(tpc_pb2.TpcTid(tid = tid))


    await asyncio.gather(response1, response2, response4, response5)

    response = await stub1.Logs(Empty())
    print("Logs S1: " + str(response))

    response = await stub2.Logs(Empty())
    print("Logs S2: " + str(response))

    response = await stub4.Logs(Empty())
    print("Logs S4: " + str(response))
    
    response = await stub5.Logs(Empty())
    print("Logs S5: " + str(response))


    

async def test_paxos() -> None:
    channel1 = grpc.aio.insecure_channel("localhost:50051")
    channel2 = grpc.aio.insecure_channel("localhost:50052")
    channel3 = grpc.aio.insecure_channel("localhost:50053")
    
    stub1 = tpc_pb2_grpc.TpcServerStub(channel1)
    stub2 = tpc_pb2_grpc.TpcServerStub(channel2)
    stub3 = tpc_pb2_grpc.TpcServerStub(channel3)

    # response = await stub1.Transfer(
    #     tpc_pb2.TransferReq(
    #         txn = tpc_pb2.Transaction(sender=0, receiver=999, amount=3),
    #         tid = 1
    #     )
    # )
    # print("Response: ", str(response))

    # response = await stub2.Transfer(
    #     tpc_pb2.TransferReq(
    #         txn = tpc_pb2.Transaction(sender=2, receiver=3, amount=2),
    #         tid = 2
    #     )
    # )
    # print("Response: ", str(response))

    # response await stub3.Transfer(
    #     tpc_pb2.TransferReq(
    #         txn = tpc_pb2.Transaction(sender=3, receiver=10, amount=4),
    #         tid = 3
    #     )
    # )
    # print("Response: ", str(response))


    response = await stub1.Logs(Empty())
    print("Logs S1: " + str(response))

    # response = await stub2.Logs(Empty())
    # print("Logs S2: " + str(response))

    response = await stub3.Logs(Empty())
    print("Logs S3: " + str(response))


async def test_tpc1() -> None:
    channel1 = grpc.aio.insecure_channel("localhost:50051")
    channel2 = grpc.aio.insecure_channel("localhost:50052")
    channel4 = grpc.aio.insecure_channel("localhost:50054")
    channel5 = grpc.aio.insecure_channel("localhost:50055")
    tid = 1

    stub1 = tpc_pb2_grpc.TpcServerStub(channel1)
    stub2 = tpc_pb2_grpc.TpcServerStub(channel2)
    stub4 = tpc_pb2_grpc.TpcServerStub(channel4)
    stub5 = tpc_pb2_grpc.TpcServerStub(channel5)

    response1 = stub1.TpcPrepare(
        tpc_pb2.TransferReq(
            txn = tpc_pb2.Transaction(sender=2, receiver=1002, amount=3),
            tid = tid
        )
    )

    response4 = stub4.TpcPrepare(
        tpc_pb2.TransferReq(
            txn = tpc_pb2.Transaction(sender=2, receiver=1002, amount=3),
            tid = tid
        )
    )

    await asyncio.gather(response1, response4)
    print("Response1: ", str(response1))
    print("Response4: ", str(response4))


    response1 = stub1.TpcCommit(tpc_pb2.TpcTid(tid = tid))
    response2 = stub2.TpcCommit(tpc_pb2.TpcTid(tid = tid))
    response4 = stub4.TpcCommit(tpc_pb2.TpcTid(tid = tid))
    response5 = stub5.TpcCommit(tpc_pb2.TpcTid(tid = tid))


    await asyncio.gather(response1, response2, response4, response5)

    response = await stub1.Logs(Empty())
    print("Logs S1: " + str(response))

    response = await stub2.Logs(Empty())
    print("Logs S2: " + str(response))

    response = await stub4.Logs(Empty())
    print("Logs S4: " + str(response))
    
    response = await stub5.Logs(Empty())
    print("Logs S5: " + str(response))


async def test_tpc2() -> None:
    channel1 = grpc.aio.insecure_channel("localhost:50051")
    channel2 = grpc.aio.insecure_channel("localhost:50052")
    channel4 = grpc.aio.insecure_channel("localhost:50054")
    channel5 = grpc.aio.insecure_channel("localhost:50055")
    tid = 1

    stub1 = tpc_pb2_grpc.TpcServerStub(channel1)
    stub2 = tpc_pb2_grpc.TpcServerStub(channel2)
    stub4 = tpc_pb2_grpc.TpcServerStub(channel4)
    stub5 = tpc_pb2_grpc.TpcServerStub(channel5)

    response1 = stub1.TpcPrepare(
        tpc_pb2.TransferReq(
            txn = tpc_pb2.Transaction(sender=2, receiver=1002, amount=3),
            tid = tid
        )
    )

    response4 = stub4.TpcPrepare(
        tpc_pb2.TransferReq(
            txn = tpc_pb2.Transaction(sender=2, receiver=1002, amount=3),
            tid = tid
        )
    )

    await asyncio.gather(response1, response4)
    print("Response1: ", str(response1))
    print("Response4: ", str(response4))


    response1 = stub1.TpcAbort(tpc_pb2.TpcTid(tid = tid))
    response2 = stub2.TpcAbort(tpc_pb2.TpcTid(tid = tid))
    response4 = stub4.TpcAbort(tpc_pb2.TpcTid(tid = tid))
    response5 = stub5.TpcAbort(tpc_pb2.TpcTid(tid = tid))


    await asyncio.gather(response1, response2, response4, response5)

    response = await stub1.Logs(Empty())
    print("Logs S1: " + str(response))

    response = await stub2.Logs(Empty())
    print("Logs S2: " + str(response))

    response = await stub4.Logs(Empty())
    print("Logs S4: " + str(response))
    
    response = await stub5.Logs(Empty())
    print("Logs S5: " + str(response))


async def test_paxos_and_tpc1() -> None:
    channel1 = grpc.aio.insecure_channel("localhost:50051")
    channel2 = grpc.aio.insecure_channel("localhost:50052")
    channel4 = grpc.aio.insecure_channel("localhost:50054")
    channel5 = grpc.aio.insecure_channel("localhost:50055")

    stub1 = tpc_pb2_grpc.TpcServerStub(channel1)
    stub2 = tpc_pb2_grpc.TpcServerStub(channel2)
    stub4 = tpc_pb2_grpc.TpcServerStub(channel4)
    stub5 = tpc_pb2_grpc.TpcServerStub(channel5)

    # Intra shard transactions
    response1 = await stub1.Transfer(
        tpc_pb2.TransferReq(
            txn = tpc_pb2.Transaction(sender=2, receiver=12, amount=2),
            tid = 1
        )
    )
    print("IS respnonse1: ", str(response1))

    response4 = await stub4.Transfer(
        tpc_pb2.TransferReq(
            txn = tpc_pb2.Transaction(sender=1002, receiver=1012, amount=3),
            tid = 2
        )
    )
    print("IS respnonse4: ", str(response4))

    # Cross shard transactions
    response1 = stub1.TpcPrepare(
        tpc_pb2.TransferReq(
            txn = tpc_pb2.Transaction(sender=3, receiver=1003, amount=1),
            tid = 3
        )
    )

    response4 = stub4.TpcPrepare(
        tpc_pb2.TransferReq(
            txn = tpc_pb2.Transaction(sender=3, receiver=1003, amount=1),
            tid = 3
        )
    )

    await asyncio.gather(response1, response4)
    print("Response1: ", str(response1))
    print("Response4: ", str(response4))


    response1 = stub1.TpcCommit(tpc_pb2.TpcTid(tid = 3))
    response2 = stub2.TpcCommit(tpc_pb2.TpcTid(tid = 3))
    response4 = stub4.TpcCommit(tpc_pb2.TpcTid(tid = 3))
    response5 = stub5.TpcCommit(tpc_pb2.TpcTid(tid = 3))


    await asyncio.gather(response1, response2, response4, response5)

    response = await stub1.Logs(Empty())
    print("Logs S1: " + str(response))

    response = await stub2.Logs(Empty())
    print("Logs S2: " + str(response))

    response = await stub4.Logs(Empty())
    print("Logs S4: " + str(response))
    
    response = await stub5.Logs(Empty())
    print("Logs S5: " + str(response))


async def test_paxos_and_tpc2() -> None:
    channel1 = grpc.aio.insecure_channel("localhost:50051")
    channel2 = grpc.aio.insecure_channel("localhost:50052")
    channel4 = grpc.aio.insecure_channel("localhost:50054")
    channel5 = grpc.aio.insecure_channel("localhost:50055")

    stub1 = tpc_pb2_grpc.TpcServerStub(channel1)
    stub2 = tpc_pb2_grpc.TpcServerStub(channel2)
    stub4 = tpc_pb2_grpc.TpcServerStub(channel4)
    stub5 = tpc_pb2_grpc.TpcServerStub(channel5)

    # Cross shard transactions
    response1 = stub1.TpcPrepare(
        tpc_pb2.TransferReq(
            txn = tpc_pb2.Transaction(sender=3, receiver=1003, amount=1),
            tid = 3
        )
    )

    response4 = stub4.TpcPrepare(
        tpc_pb2.TransferReq(
            txn = tpc_pb2.Transaction(sender=3, receiver=1003, amount=1),
            tid = 3
        )
    )

    await asyncio.gather(response1, response4)
    print("Response1: ", str(response1))
    print("Response4: ", str(response4))


    # Intra shard transactions
    response1 = await stub1.Transfer(
        tpc_pb2.TransferReq(
            txn = tpc_pb2.Transaction(sender=2, receiver=3, amount=5),
            tid = 1
        )
    )

    response4 = await stub4.Transfer(
        tpc_pb2.TransferReq(
            txn = tpc_pb2.Transaction(sender=1003, receiver=1012, amount=5),
            tid = 2
        )
    )


    response1 = stub1.TpcCommit(tpc_pb2.TpcTid(tid = 3))
    response2 = stub2.TpcCommit(tpc_pb2.TpcTid(tid = 3))
    response4 = stub4.TpcCommit(tpc_pb2.TpcTid(tid = 3))
    response5 = stub5.TpcCommit(tpc_pb2.TpcTid(tid = 3))


    await asyncio.gather(response1, response2, response4, response5)

    response = await stub1.Logs(Empty())
    print("Logs S1: " + str(response))

    response = await stub2.Logs(Empty())
    print("Logs S2: " + str(response))

    response = await stub4.Logs(Empty())
    print("Logs S4: " + str(response))
    
    response = await stub5.Logs(Empty())
    print("Logs S5: " + str(response))



if __name__ == "__main__":
    logging.basicConfig()
    # asyncio.run(test_paxos())
    # asyncio.run(test_tpc1())
    # asyncio.run(test_tpc2())
    # asyncio.run(test_paxos_and_tpc1())
    # asyncio.run(test_paxos_and_tpc2())
    asyncio.run(disconnected_tpc())