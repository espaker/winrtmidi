import hashlib, uuid, sys
NS = uuid.UUID('11f47ad5-7b73-42c0-abae-878b1e16adee').bytes
def piid(sig):
    h = bytearray(hashlib.sha1(NS + sig.encode('utf-8')).digest()[:16])
    h[6] = (h[6] & 0x0f) | 0x50; h[8] = (h[8] & 0x3f) | 0x80
    return uuid.UUID(bytes=bytes(h))
IAO='{9fc2b0bb-e446-44e2-aa61-9cab8f636af2}'; HAO='{fcdcf02c-e5d8-4478-915a-4d90b74b83a5}'
IVV='{bbe1fa4c-b0e3-4583-baef-1f1b2e483e56}'; IIT='{faa585ea-6214-4217-afda-7f46de5869b3}'; IITR='{6a79e863-4300-459a-9966-cbb660963ee1}'
TEH='{9de1c534-6ae1-11e0-84e1-18a905bcc53f}'
DI='rc(Windows.Devices.Enumeration.DeviceInformation;{aba0fb95-4398-489d-8e44-e6130927011f})'
DIC=f'rc(Windows.Devices.Enumeration.DeviceInformationCollection;pinterface({IVV};{DI}))'
IMOP='{931d6d9f-57a2-4a3a-adb8-4640886f6693}'
MIP='rc(Windows.Devices.Midi.MidiInPort;{d5c1d9db-971a-4eaf-a23d-ea19fe607ff9})'
MMR='rc(Windows.Devices.Midi.MidiMessageReceivedEventArgs;{76566e56-f328-4b51-907d-b3a8ce96bf80})'
items = {
 'IAsyncOperation_DeviceInformationCollection': f'pinterface({IAO};{DIC})',
 'IAsyncOperationCompletedHandler_DeviceInformationCollection': f'pinterface({HAO};{DIC})',
 'IAsyncOperation_DeviceInformation': f'pinterface({IAO};{DI})',
 'IAsyncOperationCompletedHandler_DeviceInformation': f'pinterface({HAO};{DI})',
 'IVectorView_DeviceInformation': f'pinterface({IVV};{DI})',
 'IIterable_DeviceInformation': f'pinterface({IIT};{DI})',
 'IIterator_DeviceInformation': f'pinterface({IITR};{DI})',
 'IAsyncOperation_IMidiOutPort': f'pinterface({IAO};{IMOP})',
 'IAsyncOperationCompletedHandler_IMidiOutPort': f'pinterface({HAO};{IMOP})',
 'IAsyncOperation_MidiInPort': f'pinterface({IAO};{MIP})',
 'IAsyncOperationCompletedHandler_MidiInPort': f'pinterface({HAO};{MIP})',
 'TypedEventHandler_MidiInPort_MidiMessageReceivedEventArgs': f'pinterface({TEH};{MIP};{MMR})',
}
for k,v in items.items():
    u = piid(v); b = u.bytes
    print(f"DEFINE_GUID(IID_{k}, 0x{b[0:4].hex()}, 0x{b[4:6].hex()}, 0x{b[6:8].hex()}, " + ", ".join(f"0x{x:02x}" for x in b[8:]) + f"); /* {u} */")
