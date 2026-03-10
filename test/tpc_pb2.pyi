from google.protobuf import empty_pb2 as _empty_pb2
from google.protobuf.internal import containers as _containers
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from typing import ClassVar as _ClassVar, Iterable as _Iterable, Mapping as _Mapping, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class Transaction(_message.Message):
    __slots__ = ("sender", "receiver", "amount")
    SENDER_FIELD_NUMBER: _ClassVar[int]
    RECEIVER_FIELD_NUMBER: _ClassVar[int]
    AMOUNT_FIELD_NUMBER: _ClassVar[int]
    sender: int
    receiver: int
    amount: int
    def __init__(self, sender: _Optional[int] = ..., receiver: _Optional[int] = ..., amount: _Optional[int] = ...) -> None: ...

class TpcTid(_message.Message):
    __slots__ = ("tid",)
    TID_FIELD_NUMBER: _ClassVar[int]
    tid: int
    def __init__(self, tid: _Optional[int] = ...) -> None: ...

class TransferReq(_message.Message):
    __slots__ = ("txn", "tid")
    TXN_FIELD_NUMBER: _ClassVar[int]
    TID_FIELD_NUMBER: _ClassVar[int]
    txn: Transaction
    tid: int
    def __init__(self, txn: _Optional[_Union[Transaction, _Mapping]] = ..., tid: _Optional[int] = ...) -> None: ...

class TransferRes(_message.Message):
    __slots__ = ("ack", "tid")
    ACK_FIELD_NUMBER: _ClassVar[int]
    TID_FIELD_NUMBER: _ClassVar[int]
    ack: bool
    tid: int
    def __init__(self, ack: bool = ..., tid: _Optional[int] = ...) -> None: ...

class BalanceReq(_message.Message):
    __slots__ = ("client",)
    CLIENT_FIELD_NUMBER: _ClassVar[int]
    client: int
    def __init__(self, client: _Optional[int] = ...) -> None: ...

class BalanceRes(_message.Message):
    __slots__ = ("amount",)
    AMOUNT_FIELD_NUMBER: _ClassVar[int]
    amount: int
    def __init__(self, amount: _Optional[int] = ...) -> None: ...

class LogRes(_message.Message):
    __slots__ = ("logs",)
    LOGS_FIELD_NUMBER: _ClassVar[int]
    logs: _containers.RepeatedCompositeFieldContainer[LogEntry]
    def __init__(self, logs: _Optional[_Iterable[_Union[LogEntry, _Mapping]]] = ...) -> None: ...

class Ballot(_message.Message):
    __slots__ = ("num", "server_id")
    NUM_FIELD_NUMBER: _ClassVar[int]
    SERVER_ID_FIELD_NUMBER: _ClassVar[int]
    num: int
    server_id: int
    def __init__(self, num: _Optional[int] = ..., server_id: _Optional[int] = ...) -> None: ...

class PrepareReq(_message.Message):
    __slots__ = ("ballot", "last_inserted")
    BALLOT_FIELD_NUMBER: _ClassVar[int]
    LAST_INSERTED_FIELD_NUMBER: _ClassVar[int]
    ballot: Ballot
    last_inserted: int
    def __init__(self, ballot: _Optional[_Union[Ballot, _Mapping]] = ..., last_inserted: _Optional[int] = ...) -> None: ...

class PrepareRes(_message.Message):
    __slots__ = ("ack", "ballot", "accept_num", "accept_val", "last_inserted", "server_id")
    ACK_FIELD_NUMBER: _ClassVar[int]
    BALLOT_FIELD_NUMBER: _ClassVar[int]
    ACCEPT_NUM_FIELD_NUMBER: _ClassVar[int]
    ACCEPT_VAL_FIELD_NUMBER: _ClassVar[int]
    LAST_INSERTED_FIELD_NUMBER: _ClassVar[int]
    SERVER_ID_FIELD_NUMBER: _ClassVar[int]
    ack: bool
    ballot: Ballot
    accept_num: Ballot
    accept_val: TransferReq
    last_inserted: int
    server_id: int
    def __init__(self, ack: bool = ..., ballot: _Optional[_Union[Ballot, _Mapping]] = ..., accept_num: _Optional[_Union[Ballot, _Mapping]] = ..., accept_val: _Optional[_Union[TransferReq, _Mapping]] = ..., last_inserted: _Optional[int] = ..., server_id: _Optional[int] = ...) -> None: ...

class AcceptReq(_message.Message):
    __slots__ = ("ballot", "r")
    BALLOT_FIELD_NUMBER: _ClassVar[int]
    R_FIELD_NUMBER: _ClassVar[int]
    ballot: Ballot
    r: TransferReq
    def __init__(self, ballot: _Optional[_Union[Ballot, _Mapping]] = ..., r: _Optional[_Union[TransferReq, _Mapping]] = ...) -> None: ...

class AcceptRes(_message.Message):
    __slots__ = ("ack", "ballot")
    ACK_FIELD_NUMBER: _ClassVar[int]
    BALLOT_FIELD_NUMBER: _ClassVar[int]
    ack: bool
    ballot: Ballot
    def __init__(self, ack: bool = ..., ballot: _Optional[_Union[Ballot, _Mapping]] = ...) -> None: ...

class CommitReq(_message.Message):
    __slots__ = ("ballot",)
    BALLOT_FIELD_NUMBER: _ClassVar[int]
    ballot: Ballot
    def __init__(self, ballot: _Optional[_Union[Ballot, _Mapping]] = ...) -> None: ...

class SyncReq(_message.Message):
    __slots__ = ("last_inserted",)
    LAST_INSERTED_FIELD_NUMBER: _ClassVar[int]
    last_inserted: int
    def __init__(self, last_inserted: _Optional[int] = ...) -> None: ...

class LogEntry(_message.Message):
    __slots__ = ("txn", "tid", "type", "status", "ballot_num", "ballot_server_id")
    TXN_FIELD_NUMBER: _ClassVar[int]
    TID_FIELD_NUMBER: _ClassVar[int]
    TYPE_FIELD_NUMBER: _ClassVar[int]
    STATUS_FIELD_NUMBER: _ClassVar[int]
    BALLOT_NUM_FIELD_NUMBER: _ClassVar[int]
    BALLOT_SERVER_ID_FIELD_NUMBER: _ClassVar[int]
    txn: Transaction
    tid: int
    type: int
    status: int
    ballot_num: int
    ballot_server_id: int
    def __init__(self, txn: _Optional[_Union[Transaction, _Mapping]] = ..., tid: _Optional[int] = ..., type: _Optional[int] = ..., status: _Optional[int] = ..., ballot_num: _Optional[int] = ..., ballot_server_id: _Optional[int] = ...) -> None: ...

class SyncRes(_message.Message):
    __slots__ = ("ack", "logs", "last_inserted_ballot")
    ACK_FIELD_NUMBER: _ClassVar[int]
    LOGS_FIELD_NUMBER: _ClassVar[int]
    LAST_INSERTED_BALLOT_FIELD_NUMBER: _ClassVar[int]
    ack: bool
    logs: _containers.RepeatedCompositeFieldContainer[LogEntry]
    last_inserted_ballot: Ballot
    def __init__(self, ack: bool = ..., logs: _Optional[_Iterable[_Union[LogEntry, _Mapping]]] = ..., last_inserted_ballot: _Optional[_Union[Ballot, _Mapping]] = ...) -> None: ...

class DisconnectReq(_message.Message):
    __slots__ = ("servers",)
    SERVERS_FIELD_NUMBER: _ClassVar[int]
    servers: _containers.RepeatedScalarFieldContainer[str]
    def __init__(self, servers: _Optional[_Iterable[str]] = ...) -> None: ...
