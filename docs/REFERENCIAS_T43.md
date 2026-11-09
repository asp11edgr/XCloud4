# T43 protocol references

The T43 RTX/FlexFEC implementation is original XCloud4 code. The following
primary references supply protocol-format facts only; no implementation is
copied or adapted from them.

- [RFC 4588, section 4](https://www.rfc-editor.org/rfc/rfc4588.html#section-4): RTX original sequence number and retransmission packet format.
- [Flexible FEC draft 03, section 5.1.1](https://datatracker.ietf.org/doc/html/draft-ietf-payload-flexible-fec-scheme-03#section-5.1.1): the draft wire format associated with `flexfec-03`, rather than the later RFC format.
- [libwebrtc FlexFEC-03 header reader/writer](https://webrtc.googlesource.com/src/+/92a030d50ec74776b8fd3278354d7607960b5dd1/modules/rtp_rtcp/source/flexfec_03_header_reader_writer.cc): header parsing starts at line 89; protected SSRC and sequence base at 111–119; mask continuation/termination at 136, 151 and 172; length at 206. The file's adjacent header contains the diagram. Its 15-, 46- and 109-bit masks use 20-, 24- and 32-byte FEC headers.
- [libwebrtc XOR recovery](https://webrtc.googlesource.com/src/+/92a030d50ec74776b8fd3278354d7607960b5dd1/modules/rtp_rtcp/source/forward_error_correction.cc): recovery initialization/finish at 556/594, header/payload XOR at 622/645, packet reconstruction at 664, and the one-missing-packet condition at 697.

The libwebrtc references are pinned to commit
`92a030d50ec74776b8fd3278354d7607960b5dd1`, distributed by its authors under
BSD-3-Clause. They are research references, not bundled source or a port.
Unsupported or invalid formats are rejected by bounded validation; advertising
an option is not evidence that Xbox negotiated or transmitted that method.
