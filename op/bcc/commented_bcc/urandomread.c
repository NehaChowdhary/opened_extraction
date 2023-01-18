/* 
 OPENED COMMENT BEGIN 
 { 
 File: /root/examples/bcc/urandomread.c,
 Startline: 1,
 Endline: 5,
 Funcname: TRACEPOINT_PROBE,
 Input: (random, urandom_read),
 Output: NA,
 Helpers: [bpf_trace_printk,],
 Read_maps: [],
 Update_maps: [],
 Func Description: TO BE ADDED,
 OpenAI Func Description: TO BE ADDED,
 Executed Command: 'python3 generate_docs.py txl_bcc',
 Commentor: TO BE ADDED (<name>,<email>) 
 } 
 OPENED COMMENT END 
 */ 
TRACEPOINT_PROBE(random, urandom_read) {
    // args is from /sys/kernel/debug/tracing/events/random/urandom_read/format
    bpf_trace_printk("%d\\n", args->got_bits);
    return 0;
}
