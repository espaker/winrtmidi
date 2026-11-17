import uuid, subprocess, sys
fixed = {
 'IUnknown_':'00000000-0000-0000-c000-000000000046',
 'IInspectable_':'af86e2e0-b12d-4c6a-9c5a-d7aa65101e90',
 'IAgileObject_':'94ea2b94-e9cc-49e0-c0ff-ee64ca8f5b90',
 'IMarshal_':'00000003-0000-0000-c000-000000000046',
 'IActivationFactory_':'00000035-0000-0000-c000-000000000046',
 'IAsyncInfo_':'00000036-0000-0000-c000-000000000046',
 'IClosable_':'30d5a829-7fa4-4026-83bb-d75bae4ea99e',
 'IBuffer_':'905a0fe0-bc53-11df-8c49-001e4fc686da',
 'IBufferByteAccess_':'905a0fef-bc53-11df-8c49-001e4fc686da',
 'IDataReader_':'e2b50029-b4c1-4314-a4b8-fb813a2f275e',
 'IDataReaderStatics_':'11fcbfc8-f93a-471b-b121-f379e349313c',
 'IDataWriter_':'64b89265-d341-4922-b38a-dd4af8808c4e',
 'IDeviceInformation_':'aba0fb95-4398-489d-8e44-e6130927011f',
 'IDeviceInformationStatics_':'c17f100e-3a46-4a78-8013-769dc9b97390',
 'IMidiOutPort_':'931d6d9f-57a2-4a3a-adb8-4640886f6693',
 'IMidiOutPortStatics_':'065cc3e9-0f88-448b-9b64-a95826c65b8f',
 'IMidiInPort_':'d5c1d9db-971a-4eaf-a23d-ea19fe607ff9',
 'IMidiInPortStatics_':'44c439dc-67ff-4a6e-8bac-fdb6610cf296',
 'IMidiMessageReceivedEventArgs_':'76566e56-f328-4b51-907d-b3a8ce96bf80',
 'IMidiMessage_':'79767945-1094-4283-9be0-289fc0ee8334',
}
out = subprocess.run([sys.executable, '-I', 'piid.py'], capture_output=True, text=True).stdout
for line in out.splitlines():
    name = line.split('(IID_')[1].split(',')[0]; u = line.split('/* ')[1].split(' */')[0]
    fixed[name] = u
print('/* gerado por genguids.py */')
for k, v in fixed.items():
    b = uuid.UUID(v).bytes
    print(f"static const GUID IID_{k} = {{0x{b[0:4].hex()}, 0x{b[4:6].hex()}, 0x{b[6:8].hex()}, {{" + ", ".join(f"0x{x:02x}" for x in b[8:]) + f"}}}}; /* {v} */")
