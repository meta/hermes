/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// REQUIRES: contrib_extensions
// RUN: LC_ALL=en_US.UTF-8 %hermes -O -target=HBC %s | %FileCheck --match-full-lines %s
"use strict";

print('TextDecoder');
// CHECK-LABEL: TextDecoder

var decoder = new TextDecoder();
print(Object.prototype.toString.call(decoder));
// CHECK-NEXT: [object TextDecoder]

// Test default properties
print(decoder.encoding);
// CHECK-NEXT: utf-8
print(decoder.fatal);
// CHECK-NEXT: false
print(decoder.ignoreBOM);
// CHECK-NEXT: false

// Test property descriptors
const descEncoding = Object.getOwnPropertyDescriptor(TextDecoder.prototype, 'encoding');
print(descEncoding.enumerable);
// CHECK-NEXT: true
print(descEncoding.configurable);
// CHECK-NEXT: true

const descFatal = Object.getOwnPropertyDescriptor(TextDecoder.prototype, 'fatal');
print(descFatal.enumerable);
// CHECK-NEXT: true
print(descFatal.configurable);
// CHECK-NEXT: true

const descIgnoreBOM = Object.getOwnPropertyDescriptor(TextDecoder.prototype, 'ignoreBOM');
print(descIgnoreBOM.enumerable);
// CHECK-NEXT: true
print(descIgnoreBOM.configurable);
// CHECK-NEXT: true

// Test constructor options
var decoderWithOptions = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true });
print(decoderWithOptions.encoding);
// CHECK-NEXT: utf-8
print(decoderWithOptions.fatal);
// CHECK-NEXT: true
print(decoderWithOptions.ignoreBOM);
// CHECK-NEXT: true

// Test encoding labels
var utf8Decoder = new TextDecoder('utf8');
print(utf8Decoder.encoding);
// CHECK-NEXT: utf-8

var utf16leDecoder = new TextDecoder('utf-16le');
print(utf16leDecoder.encoding);
// CHECK-NEXT: utf-16le

var utf16beDecoder = new TextDecoder('utf-16be');
print(utf16beDecoder.encoding);
// CHECK-NEXT: utf-16be

var windows1252Decoder = new TextDecoder('windows-1252');
print(windows1252Decoder.encoding);
// CHECK-NEXT: windows-1252

// All these aliases should map to windows-1252
var latin1Decoder = new TextDecoder('latin1');
print(latin1Decoder.encoding);
// CHECK-NEXT: windows-1252

var asciiDecoder = new TextDecoder('ascii');
print(asciiDecoder.encoding);
// CHECK-NEXT: windows-1252

var iso88591Decoder = new TextDecoder('iso-8859-1');
print(iso88591Decoder.encoding);
// CHECK-NEXT: windows-1252

// Test unknown encoding
try {
  new TextDecoder('unknown-encoding');
} catch (e) {
  print(e.name);
  // CHECK-NEXT: RangeError
}

// Test decoding empty input
print(decoder.decode() === '');
// CHECK-NEXT: true

print(decoder.decode(undefined) === '');
// CHECK-NEXT: true

print(decoder.decode(new Uint8Array(0)) === '');
// CHECK-NEXT: true

// Test basic UTF-8 decoding
var utf8Bytes = new Uint8Array([116, 101, 115, 116]); // "test"
print(decoder.decode(utf8Bytes));
// CHECK-NEXT: test

// Test UTF-8 with multi-byte characters
// "↑↓" = U+2191 U+2193 = E2 86 91 E2 86 93
var arrowBytes = new Uint8Array([0xE2, 0x86, 0x91, 0xE2, 0x86, 0x93]);
print(decoder.decode(arrowBytes));
// CHECK-NEXT: ↑↓

// Test UTF-8 with 4-byte character (emoji)
// "😃" = U+1F603 = F0 9F 98 83
var emojiBytes = new Uint8Array([0xF0, 0x9F, 0x98, 0x83]);
print(decoder.decode(emojiBytes));
// CHECK-NEXT: 😃

// Test UTF-8 BOM handling (default: skip BOM)
var utf8WithBOM = new Uint8Array([0xEF, 0xBB, 0xBF, 116, 101, 115, 116]);
print(decoder.decode(utf8WithBOM));
// CHECK-NEXT: test

// Test UTF-8 BOM with ignoreBOM=true (include BOM in output)
var decoderIgnoreBOM = new TextDecoder('utf-8', { ignoreBOM: true });
var result = decoderIgnoreBOM.decode(utf8WithBOM);
print(result.length);
// CHECK-NEXT: 5

// Test invalid UTF-8 handling (non-fatal, replacement character)
var invalidUtf8 = new Uint8Array([0x80, 0x81, 116, 101, 115, 116]);
var decoded = decoder.decode(invalidUtf8);
print(decoded.length);
// CHECK-NEXT: 6
print(decoded.charCodeAt(0));
// CHECK-NEXT: 65533
print(decoded.charCodeAt(1));
// CHECK-NEXT: 65533

// Test fatal mode for invalid UTF-8
var fatalDecoder = new TextDecoder('utf-8', { fatal: true });
try {
  fatalDecoder.decode(new Uint8Array([0x80]));
} catch (e) {
  print(e.name);
  // CHECK-NEXT: TypeError
}

// Valid incomplete sequences (1 replacement char for entire sequence)
print(new TextDecoder().decode(Uint8Array.of(0xF0)) === '\uFFFD');
// CHECK-NEXT: true
print(new TextDecoder().decode(Uint8Array.of(0xF0, 0x9F)) === '\uFFFD');
// CHECK-NEXT: true
print(new TextDecoder().decode(Uint8Array.of(0xF0, 0x9F, 0x92)) === '\uFFFD');
// CHECK-NEXT: true
print(new TextDecoder().decode(Uint8Array.of(0xF0, 0x90, 0x80)).length);
// CHECK-NEXT: 1

// Invalid sequences (replacement char per maximal subpart)
print(new TextDecoder().decode(Uint8Array.of(0xF0, 0x9F, 0x41)) === '\uFFFDA');
// CHECK-NEXT: true
print(new TextDecoder().decode(Uint8Array.of(0xF0, 0x41, 0x42)) === '\uFFFDAB');
// CHECK-NEXT: true
print(new TextDecoder().decode(Uint8Array.of(0xF0, 0x41, 0xF0)) === '\uFFFDA\uFFFD');
// CHECK-NEXT: true
print(new TextDecoder().decode(Uint8Array.of(0xF0, 0x8F, 0x92)) === '\uFFFD\uFFFD\uFFFD');
// CHECK-NEXT: true
// 0xF0 requires second byte 0x90-0xBF, so 0x80 is invalid - 3 replacements
print(new TextDecoder().decode(Uint8Array.of(0xF0, 0x80, 0x80)).length);
// CHECK-NEXT: 3

// Test UTF-16LE decoding
// "test" in UTF-16LE = 74 00 65 00 73 00 74 00
var utf16leBytes = new Uint8Array([0x74, 0x00, 0x65, 0x00, 0x73, 0x00, 0x74, 0x00]);
print(utf16leDecoder.decode(utf16leBytes));
// CHECK-NEXT: test

// Test UTF-16LE with BOM (FF FE)
var utf16leWithBOM = new Uint8Array([0xFF, 0xFE, 0x74, 0x00, 0x65, 0x00, 0x73, 0x00, 0x74, 0x00]);
print(utf16leDecoder.decode(utf16leWithBOM));
// CHECK-NEXT: test

// Test UTF-16BE decoding
// "test" in UTF-16BE = 00 74 00 65 00 73 00 74
var utf16beBytes = new Uint8Array([0x00, 0x74, 0x00, 0x65, 0x00, 0x73, 0x00, 0x74]);
print(utf16beDecoder.decode(utf16beBytes));
// CHECK-NEXT: test

// Test UTF-16BE with BOM (FE FF)
var utf16beWithBOM = new Uint8Array([0xFE, 0xFF, 0x00, 0x74, 0x00, 0x65, 0x00, 0x73, 0x00, 0x74]);
print(utf16beDecoder.decode(utf16beWithBOM));
// CHECK-NEXT: test

// Test Windows-1252 decoding
// "café" = 63 61 66 E9
var cafeBytes = new Uint8Array([0x63, 0x61, 0x66, 0xE9]);
print(windows1252Decoder.decode(cafeBytes));
// CHECK-NEXT: café

// Test Windows-1252 with high bytes (0xA0-0xFF map directly like Latin-1)
var highBytes = new Uint8Array([0xA9, 0xAE, 0xB0]); // ©®°
print(windows1252Decoder.decode(highBytes));
// CHECK-NEXT: ©®°

// Test Windows-1252 special characters in 0x80-0x9F range
// 0x80 = € (U+20AC), 0x89 = ‰ (U+2030), 0x99 = ™ (U+2122)
var specialBytes = new Uint8Array([0x80, 0x89, 0x99]);
var specialDecoded = windows1252Decoder.decode(specialBytes);
print(specialDecoded.charCodeAt(0) === 0x20AC);  // €
// CHECK-NEXT: true
print(specialDecoded.charCodeAt(1) === 0x2030);  // ‰
// CHECK-NEXT: true
print(specialDecoded.charCodeAt(2) === 0x2122);  // ™
// CHECK-NEXT: true

// Test full Windows-1252 decode per WHATWG spec example
// [0x63, 0x61, 0x66, 0xE9, 0x20, 0xFF, 0x80, 0x89] = "café ÿ€‰"
var fullTest = new Uint8Array([0x63, 0x61, 0x66, 0xE9, 0x20, 0xFF, 0x80, 0x89]);
print(windows1252Decoder.decode(fullTest));
// CHECK-NEXT: café ÿ€‰

// Verify all aliases decode Windows-1252 correctly
print(latin1Decoder.decode(fullTest));
// CHECK-NEXT: café ÿ€‰
print(asciiDecoder.decode(fullTest));
// CHECK-NEXT: café ÿ€‰

// Test decoding with ArrayBuffer
var buffer = new ArrayBuffer(4);
var view = new Uint8Array(buffer);
view[0] = 116; view[1] = 101; view[2] = 115; view[3] = 116;
print(decoder.decode(buffer));
// CHECK-NEXT: test

// Test decoding with DataView
var dataView = new DataView(buffer);
print(decoder.decode(dataView));
// CHECK-NEXT: test

// Test decoding with DataView slice
var largeBuffer = new ArrayBuffer(10);
var largeView = new Uint8Array(largeBuffer);
largeView[2] = 116; largeView[3] = 101; largeView[4] = 115; largeView[5] = 116;
var sliceDataView = new DataView(largeBuffer, 2, 4);
print(decoder.decode(sliceDataView));
// CHECK-NEXT: test

// Test error handling for non-TextDecoder object
try {
  const b = {};
  TextDecoder.prototype.decode.call(b, new Uint8Array(0));
} catch (e) {
  print(e.message);
  // CHECK-NEXT: TextDecoder.prototype.decode() called on non-TextDecoder object
}

try {
  TextDecoder.prototype.decode.call(undefined, new Uint8Array(0));
} catch (e) {
  print(e.message);
  // CHECK-NEXT: TextDecoder.prototype.decode() called on non-TextDecoder object
}

// Test error handling for encoding getter
try {
  TextDecoder.prototype.encoding;
} catch (e) {
  print(e.message);
  // CHECK-NEXT: TextDecoder.prototype.encoding called on non-TextDecoder object
}

// Test error handling for fatal getter
try {
  TextDecoder.prototype.fatal;
} catch (e) {
  print(e.message);
  // CHECK-NEXT: TextDecoder.prototype.fatal called on non-TextDecoder object
}

// Test error handling for ignoreBOM getter
try {
  TextDecoder.prototype.ignoreBOM;
} catch (e) {
  print(e.message);
  // CHECK-NEXT: TextDecoder.prototype.ignoreBOM called on non-TextDecoder object
}

// Test that TextDecoder must be called as a constructor
try {
  TextDecoder();
} catch (e) {
  print(e.message);
  // CHECK-NEXT: TextDecoder must be called as a constructor
}

// Test roundtrip with TextEncoder
var encoder = new TextEncoder();
var originalText = "Hello, 世界! 🌍";
var encoded = encoder.encode(originalText);
var decoded = decoder.decode(encoded);
print(decoded === originalText);
// CHECK-NEXT: true

// Test long string with 3-byte char + emojis (to test chunk boundaries)
// "中" = U+4E2D = 3 UTF-8 bytes, 1 UTF-16 code unit
// "😀" = U+1F600 = 4 UTF-8 bytes, 2 UTF-16 code units (surrogate pair)
// 255 "中" fills buffer to 255, then emoji needs 2 slots - tests boundary
var boundaryTest = "中".repeat(255) + "😀".repeat(150);
var boundaryEncoded = encoder.encode(boundaryTest);
print(boundaryEncoded.length);  // 255*3 + 150*4 = 765 + 600 = 1365
// CHECK-NEXT: 1365
var boundaryDecoded = decoder.decode(boundaryEncoded);
print(boundaryDecoded === boundaryTest);
// CHECK-NEXT: true
print(boundaryDecoded.length);  // 255 + 150*2 = 555
// CHECK-NEXT: 555

// Test long emoji string with UTF-16LE
var utf16leEmojis = new Uint8Array(300 * 4);  // 300 emojis * 2 code units * 2 bytes
for (var i = 0; i < 300; i++) {
  // 😀 = U+1F600 = surrogate pair D83D DE00
  utf16leEmojis[i * 4 + 0] = 0x3D;  // D83D low byte
  utf16leEmojis[i * 4 + 1] = 0xD8;  // D83D high byte
  utf16leEmojis[i * 4 + 2] = 0x00;  // DE00 low byte
  utf16leEmojis[i * 4 + 3] = 0xDE;  // DE00 high byte
}
var utf16leEmojiDecoded = utf16leDecoder.decode(utf16leEmojis);
print(utf16leEmojiDecoded === "😀".repeat(300));
// CHECK-NEXT: true

// Test long emoji string with UTF-16BE
var utf16beEmojis = new Uint8Array(300 * 4);
for (var i = 0; i < 300; i++) {
  // 😀 = U+1F600 = surrogate pair D83D DE00
  utf16beEmojis[i * 4 + 0] = 0xD8;  // D83D high byte
  utf16beEmojis[i * 4 + 1] = 0x3D;  // D83D low byte
  utf16beEmojis[i * 4 + 2] = 0xDE;  // DE00 high byte
  utf16beEmojis[i * 4 + 3] = 0x00;  // DE00 low byte
}
var utf16beEmojiDecoded = utf16beDecoder.decode(utf16beEmojis);
print(utf16beEmojiDecoded === "😀".repeat(300));
// CHECK-NEXT: true

// Test unpaired surrogate in UTF-16LE (non-fatal should produce U+FFFD)
// Lone high surrogate D800 followed by 'A'
var loneHighLE = new Uint8Array([0x00, 0xD8, 0x41, 0x00]);
var loneHighResultLE = utf16leDecoder.decode(loneHighLE);
print(loneHighResultLE.charCodeAt(0) === 0xFFFD);  // Should be replacement char
// CHECK-NEXT: true
print(loneHighResultLE.charAt(1));  // Should be 'A'
// CHECK-NEXT: A

// Test unpaired surrogate in UTF-16BE (non-fatal should produce U+FFFD)
// Lone high surrogate D800 followed by 'A'
var loneHighBE = new Uint8Array([0xD8, 0x00, 0x00, 0x41]);
var loneHighResultBE = utf16beDecoder.decode(loneHighBE);
print(loneHighResultBE.charCodeAt(0) === 0xFFFD);  // Should be replacement char
// CHECK-NEXT: true
print(loneHighResultBE.charAt(1));  // Should be 'A'
// CHECK-NEXT: A

// Test long UTF-8 sequence (>256 chars to test chunking)
var longText = "a".repeat(300) + "😀" + "b".repeat(300);
var longEncoded = encoder.encode(longText);
var longDecoded = decoder.decode(longEncoded);
print(longDecoded === longText);
// CHECK-NEXT: true
print(longDecoded.length);
// CHECK-NEXT: 602

// ==================== Streaming Tests ====================

// Test UTF-8 streaming with incomplete multi-byte sequence
// "日" = U+65E5 = E6 97 A5 (3 bytes)
var streamDecoder = new TextDecoder('utf-8');
var chunk1 = new Uint8Array([0xE6, 0x97]);  // Incomplete 3-byte seq
var chunk2 = new Uint8Array([0xA5]);        // Complete it

var streamResult1 = streamDecoder.decode(chunk1, {stream: true});
print(streamResult1.length);  // Should be 0 (bytes pending)
// CHECK-NEXT: 0

var streamResult2 = streamDecoder.decode(chunk2);  // End stream
print(streamResult2);  // Should be "日"
// CHECK-NEXT: 日

// Test UTF-8 streaming with 4-byte emoji split
// "😀" = U+1F600 = F0 9F 98 80 (4 bytes)
var emojiStreamDecoder = new TextDecoder('utf-8');
var emojiChunk1 = new Uint8Array([0xF0, 0x9F, 0x98]);  // First 3 bytes
var emojiChunk2 = new Uint8Array([0x80]);              // Last byte

var emojiResult1 = emojiStreamDecoder.decode(emojiChunk1, {stream: true});
print(emojiResult1.length);  // Should be 0
// CHECK-NEXT: 0

var emojiResult2 = emojiStreamDecoder.decode(emojiChunk2);
print(emojiResult2);  // Should be "😀"
// CHECK-NEXT: 😀

// Test UTF-16LE streaming with odd byte
var utf16StreamDecoder = new TextDecoder('utf-16le');
var utf16Chunk1 = new Uint8Array([0x41]);  // Half of 'A' (0x0041)
var utf16Chunk2 = new Uint8Array([0x00]);  // Complete 'A'

var utf16Result1 = utf16StreamDecoder.decode(utf16Chunk1, {stream: true});
print(utf16Result1.length);  // Should be 0
// CHECK-NEXT: 0

var utf16Result2 = utf16StreamDecoder.decode(utf16Chunk2);
print(utf16Result2);  // Should be 'A'
// CHECK-NEXT: A

// Test UTF-16LE streaming with surrogate pair split
var utf16SurrogateDecoder = new TextDecoder('utf-16le');
// "😀" = D83D DE00 in UTF-16
var surrogateChunk1 = new Uint8Array([0x3D, 0xD8]);  // High surrogate D83D
var surrogateChunk2 = new Uint8Array([0x00, 0xDE]);  // Low surrogate DE00

var surrogateResult1 = utf16SurrogateDecoder.decode(surrogateChunk1, {stream: true});
print(surrogateResult1.length);  // Should be 0 (high surrogate pending)
// CHECK-NEXT: 0

var surrogateResult2 = utf16SurrogateDecoder.decode(surrogateChunk2);
print(surrogateResult2);  // Should be "😀"
// CHECK-NEXT: 😀

// Test that stream:false at end flushes pending bytes with replacement
var flushDecoder = new TextDecoder('utf-8');
var incompleteSeq = new Uint8Array([0xE6, 0x97]);  // Incomplete 3-byte
var flushResult = flushDecoder.decode(incompleteSeq, {stream: true});
print(flushResult.length);  // Empty during stream
// CHECK-NEXT: 0

var finalResult = flushDecoder.decode(new Uint8Array(0));  // End stream with no data
print(finalResult.charCodeAt(0) === 0xFFFD);  // Should emit replacement char
// CHECK-NEXT: true

// Test BOM handling with streaming - BOM should only be stripped once
var bomStreamDecoder = new TextDecoder('utf-8');
var bomChunk1 = new Uint8Array([0xEF, 0xBB, 0xBF, 0x41]);  // BOM + 'A'
var bomChunk2 = new Uint8Array([0xEF, 0xBB, 0xBF, 0x42]);  // BOM sequence (U+FEFF) + 'B'

var bomResult1 = bomStreamDecoder.decode(bomChunk1, {stream: true});
print(bomResult1);  // Should be 'A' (BOM stripped from first chunk)
// CHECK-NEXT: A

var bomResult2 = bomStreamDecoder.decode(bomChunk2);
// Second BOM sequence decodes to U+FEFF (not stripped because BOM already seen)
print(bomResult2.length);  // Should be 2 (U+FEFF + 'B')
// CHECK-NEXT: 2

// Test large inputs
print(new TextDecoder().decode(new Uint8Array(1e6)).length);
// CHECK-NEXT: 1000000
print(new TextDecoder().decode(new Uint8Array(2e6)).length);
// CHECK-NEXT: 2000000
print(new TextDecoder('utf-16le').decode(new Uint16Array(1e6)).length);
// CHECK-NEXT: 1000000

// Test invalid UTF-8 sequences in streaming mode (should not buffer invalid bytes)
// Invalid lead bytes should emit replacement char immediately
print(new TextDecoder().decode(Uint8Array.of(0xc0), {stream:true}).length);
// CHECK-NEXT: 1
print(new TextDecoder().decode(Uint8Array.of(0xc1), {stream:true}).length);
// CHECK-NEXT: 1
print(new TextDecoder().decode(Uint8Array.of(0xf5), {stream:true}).length);
// CHECK-NEXT: 1
print(new TextDecoder().decode(Uint8Array.of(0xff), {stream:true}).length);
// CHECK-NEXT: 1

// ED A0-BF would produce surrogates - should emit replacement chars, not buffer
print(new TextDecoder().decode(Uint8Array.of(0xed, 0xa0), {stream:true}).length);
// CHECK-NEXT: 2
print(new TextDecoder().decode(Uint8Array.of(0xed, 0xbf), {stream:true}).length);
// CHECK-NEXT: 2

// But ED 80-9F is valid start, should buffer
print(new TextDecoder().decode(Uint8Array.of(0xed, 0x9f), {stream:true}).length);
// CHECK-NEXT: 0

// F0 80-8F would be overlong - should emit replacement chars
print(new TextDecoder().decode(Uint8Array.of(0xf0, 0x80), {stream:true}).length);
// CHECK-NEXT: 2

// F4 90+ would be > U+10FFFF - should emit replacement chars
print(new TextDecoder().decode(Uint8Array.of(0xf4, 0x90), {stream:true}).length);
// CHECK-NEXT: 2

// ISO-8859-7 (Greek): ΣΑΒΒΟΠΟΥΛΟΣ
var greekDecoder = new TextDecoder('iso-8859-7');
print(greekDecoder.decode(new Uint8Array([0xD3, 0xC1, 0xC2, 0xC2, 0xCF, 0xD0, 0xCF, 0xD5, 0xCB, 0xCF, 0xD3])));
// CHECK-NEXT: ΣΑΒΒΟΠΟΥΛΟΣ

// BOM split across streaming calls should still be stripped
var bomStreamDecoder = new TextDecoder();
bomStreamDecoder.decode(new Uint8Array([0xEF, 0xBB]), { stream: true });
print(bomStreamDecoder.decode(new Uint8Array([0xBF, 0x40])).length);
// CHECK-NEXT: 1

// Test that decoder state is reset after error when stream=false
// This tests that a failed decode() with stream=false clears pending bytes
// so subsequent calls start fresh.
var stateResetDecoder = new TextDecoder('utf-16le', { fatal: true });
stateResetDecoder.decode(Uint8Array.of(0), { stream: true });  // Leaves 1 pending byte
try {
  stateResetDecoder.decode();  // Non-stream: errors with odd byte count, should reset state
} catch (e) {}
// This should NOT throw - state should be reset despite previous error
print(stateResetDecoder.decode().length);
// CHECK-NEXT: 0

// Test non-literal strings as encoding labels
// The encoding label should work the same whether it's a literal or computed string
var computedLabel = 'UTF-8'.toLowerCase();
var computedDecoder = new TextDecoder(computedLabel);
print(computedDecoder.encoding);
// CHECK-NEXT: utf-8

var joinedLabel = ['utf', '8'].join('-');
var joinedDecoder = new TextDecoder(joinedLabel);
print(joinedDecoder.encoding);
// CHECK-NEXT: utf-8

// Test that BOM state is reset after a non-streaming decode completes
// When stream=false (default), the decoder should reset its internal state including bomSeen
// So the next decode call starts fresh and should strip the BOM again.
var bomResetDecoder = new TextDecoder();
// First decode with stream=true, no BOM
bomResetDecoder.decode(Uint8Array.of(0x00, 0x00), { stream: true });
// End the stream (stream=false), this should reset all state including bomSeen
bomResetDecoder.decode(Uint8Array.of(0x00));
// Now a new decode call with BOM should strip it (fresh state)
var bomResetResult = bomResetDecoder.decode(Uint8Array.of(0xEF, 0xBB, 0xBF));
print(bomResetResult.length);  // BOM should be stripped, result should be empty
// CHECK-NEXT: 0

// Variant: Start new stream, BOM split across calls should still be stripped
var bomResetDecoder2 = new TextDecoder();
bomResetDecoder2.decode(Uint8Array.of(0x41), { stream: true });  // 'A'
bomResetDecoder2.decode();  // End stream, resets state
// New stream starting with partial BOM
bomResetDecoder2.decode(Uint8Array.of(0xEF, 0xBB), { stream: true });
print(bomResetDecoder2.decode(Uint8Array.of(0xBF)).length);  // BOM stripped, empty
// CHECK-NEXT: 0

// Test UTF-16LE with lone high surrogate followed by odd trailing byte
// Bytes [0x00, 0xD8] = code unit 0xD800 (high surrogate)
// Byte [0xD8] = incomplete trailing byte
// Result should be 1 replacement character (the incomplete surrogate pair merges with odd byte)
var utf16OddDecoder = new TextDecoder('utf-16le');
print(utf16OddDecoder.decode(Uint8Array.of(0x00, 0xD8, 0xD8)).length);
// CHECK-NEXT: 1

// WebIDL reports a bad receiver and a non-BufferSource input as a TypeError,
// like the fatal-mode case above. An unknown encoding label stays a RangeError.
function nameOfThrow(f) {
  try {
    f();
    return 'no throw';
  } catch (e) {
    return e.name;
  }
}
print(nameOfThrow(function () {
  TextDecoder.prototype.decode.call({}, new Uint8Array(1));
}));
// CHECK-NEXT: TypeError
print(nameOfThrow(function () { new TextDecoder().decode({}); }));
// CHECK-NEXT: TypeError
print(nameOfThrow(function () { new TextDecoder().decode(123); }));
// CHECK-NEXT: TypeError
print(nameOfThrow(function () { new TextDecoder().decode('str'); }));
// CHECK-NEXT: TypeError
print(nameOfThrow(function () { new TextDecoder('bogus-enc'); }));
// CHECK-NEXT: RangeError
