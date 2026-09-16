from pc_host.protocol import PCM_BYTES, crc16_ccitt, pack_voice_frame, unpack_voice_frame, wav_header

def test_voice_frame_roundtrip():
    pcm = b"\x01\x02" * (PCM_BYTES // 2)
    assert unpack_voice_frame(pack_voice_frame(65537, 9, pcm)).seq == 1

def test_crc_and_wav_header():
    assert crc16_ccitt(b"123456789") == 0x29B1
    assert wav_header(640)[0:4] == b"RIFF" and len(wav_header(640)) == 44
