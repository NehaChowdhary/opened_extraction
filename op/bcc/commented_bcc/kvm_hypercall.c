#define EXIT_REASON 18
BPF_HASH(start, u8, u8);

/* 
 OPENED COMMENT BEGIN 
 { 
 File: /root/examples/bcc/kvm_hypercall.c,
 Startline: 4,
 Endline: 12,
 Funcname: TRACEPOINT_PROBE,
 Input: (kvm, kvm_exit),
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
TRACEPOINT_PROBE(kvm, kvm_exit) {
    u8 e = EXIT_REASON;
    u8 one = 1;
    if (args->exit_reason == EXIT_REASON) {
        bpf_trace_printk("KVM_EXIT exit_reason : %d\\n", args->exit_reason);
        start.update(&e, &one);
    }
    return 0;
}

/* 
 OPENED COMMENT BEGIN 
 { 
 File: /root/examples/bcc/kvm_hypercall.c,
 Startline: 14,
 Endline: 23,
 Funcname: TRACEPOINT_PROBE,
 Input: (kvm, kvm_entry),
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
TRACEPOINT_PROBE(kvm, kvm_entry) {
    u8 e = EXIT_REASON;
    u8 zero = 0;
    u8 *s = start.lookup(&e);
    if (s != NULL && *s == 1) {
        bpf_trace_printk("KVM_ENTRY vcpu_id : %u\\n", args->vcpu_id);
        start.update(&e, &zero);
    }
    return 0;
}

/* 
 OPENED COMMENT BEGIN 
 { 
 File: /root/examples/bcc/kvm_hypercall.c,
 Startline: 25,
 Endline: 33,
 Funcname: TRACEPOINT_PROBE,
 Input: (kvm, kvm_hypercall),
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
TRACEPOINT_PROBE(kvm, kvm_hypercall) {
    u8 e = EXIT_REASON;
    u8 zero = 0;
    u8 *s = start.lookup(&e);
    if (s != NULL && *s == 1) {
        bpf_trace_printk("HYPERCALL nr : %d\\n", args->nr);
    }
    return 0;
};
