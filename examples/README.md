# examples

Some ready-2-use/ready-2-extend examples/utils.
All examples are prefixed with their used LANG.

## c-captured

A capture daemon suitable for low-resource devices.
It saves flows that were guessed/undetected/risky/midstream to a PCAP file for manual analysis.
Basicially a combination of `py-flow-undetected-to-pcap` and `py-risky-flow-to-pcap`.

## c-collectd

A collecd-exec compatible middleware that gathers statistic values from nDPId.

## c-json-stdout

Tiny nDPId json dumper. Does not provide any useful funcationality besides dumping parsed JSON objects.

## go-dashboard

A discontinued tty/ncurses nDPId dashboard. I've figured out that Go + NCurses is a bad idea.

## py-flow-info

Prints prettyfied information about flow events.

## py-flow-undetected-to-pcap

Captures and saves undetected flows to a PCAP file.

## py-json-stdout

Dump received and parsed JSON strings.

## py-risky-flow-to-pcap

Captures and saves risky flows to a PCAP file.

## py-schema-validation

Validate nDPId JSON strings against pre-defined JSON schema's.
See `schema/`.