"""SH-4 (SH7750/SH7091) instruction decoder. Own implementation from the SH-4
programming manual encodings. decode(word) -> Insn or None (invalid/reserved)."""
import struct

# pattern letters: n=Rn, m=Rm, i=imm, d=disp, 0/1 fixed
TABLE = [
 # fixed
 ("0000000000001000","clrt"),("0000000000101000","clrmac"),("0000000001001000","clrs"),
 ("0000000000011000","sett"),("0000000001011000","sets"),("0000000000001001","nop"),
 ("0000000000011011","sleep"),("0000000000101011","rte"),("0000000000001011","rts"),
 ("0000000000111000","ldtlb"),("0000000000011001","div0u"),
 ("1111001111111101","fschg"),("1111101111111101","frchg"),
 # 0000 group
 ("0000nnnn00000010","stc_sr"),("0000nnnn00010010","stc_gbr"),("0000nnnn00100010","stc_vbr"),
 ("0000nnnn00110010","stc_ssr"),("0000nnnn01000010","stc_spc"),("0000nnnn00111010","stc_sgr"),
 ("0000nnnn11111010","stc_dbr"),("0000nnnn1mmm0010","stc_bank"),
 ("0000nnnn00001010","sts_mach"),("0000nnnn00011010","sts_macl"),("0000nnnn00101010","sts_pr"),
 ("0000nnnn01011010","sts_fpul"),("0000nnnn01101010","sts_fpscr"),
 ("0000nnnn00101001","movt"),("0000nnnn10000011","pref"),("0000nnnn10010011","ocbi"),
 ("0000nnnn10100011","ocbp"),("0000nnnn10110011","ocbwb"),("0000nnnn11000011","movca"),
 ("0000mmmm00100011","braf"),("0000mmmm00000011","bsrf"),
 ("0000nnnnmmmm0100","movb_s0"),("0000nnnnmmmm0101","movw_s0"),("0000nnnnmmmm0110","movl_s0"),
 ("0000nnnnmmmm0111","mull"),
 ("0000nnnnmmmm1100","movb_l0"),("0000nnnnmmmm1101","movw_l0"),("0000nnnnmmmm1110","movl_l0"),
 ("0000nnnnmmmm1111","macl"),
 ("0001nnnnmmmmdddd","movl_s4"),
 # 0010
 ("0010nnnnmmmm0000","movb_s"),("0010nnnnmmmm0001","movw_s"),("0010nnnnmmmm0010","movl_s"),
 ("0010nnnnmmmm0100","movb_m"),("0010nnnnmmmm0101","movw_m"),("0010nnnnmmmm0110","movl_m"),
 ("0010nnnnmmmm0111","div0s"),("0010nnnnmmmm1000","tst"),("0010nnnnmmmm1001","and"),
 ("0010nnnnmmmm1010","xor"),("0010nnnnmmmm1011","or"),("0010nnnnmmmm1100","cmpstr"),
 ("0010nnnnmmmm1101","xtrct"),("0010nnnnmmmm1110","muluw"),("0010nnnnmmmm1111","mulsw"),
 # 0011
 ("0011nnnnmmmm0000","cmpeq"),("0011nnnnmmmm0010","cmphs"),("0011nnnnmmmm0011","cmpge"),
 ("0011nnnnmmmm0100","div1"),("0011nnnnmmmm0101","dmulu"),("0011nnnnmmmm0110","cmphi"),
 ("0011nnnnmmmm0111","cmpgt"),("0011nnnnmmmm1000","sub"),("0011nnnnmmmm1010","subc"),
 ("0011nnnnmmmm1011","subv"),("0011nnnnmmmm1100","add"),("0011nnnnmmmm1101","dmuls"),
 ("0011nnnnmmmm1110","addc"),("0011nnnnmmmm1111","addv"),
 # 0100
 ("0100nnnn00000000","shll"),("0100nnnn00000001","shlr"),("0100nnnn00000010","stsl_mach"),
 ("0100nnnn00000011","stcl_sr"),("0100nnnn00000100","rotl"),("0100nnnn00000101","rotr"),
 ("0100mmmm00000110","ldsl_mach"),("0100mmmm00000111","ldcl_sr"),("0100nnnn00001000","shll2"),
 ("0100nnnn00001001","shlr2"),("0100mmmm00001010","lds_mach"),("0100mmmm00001011","jsr"),
 ("0100mmmm00001110","ldc_sr"),
 ("0100nnnn00010000","dt"),("0100nnnn00010001","cmppz"),("0100nnnn00010010","stsl_macl"),
 ("0100nnnn00010011","stcl_gbr"),("0100nnnn00010101","cmppl"),("0100mmmm00010110","ldsl_macl"),
 ("0100mmmm00010111","ldcl_gbr"),("0100nnnn00011000","shll8"),("0100nnnn00011001","shlr8"),
 ("0100mmmm00011010","lds_macl"),("0100nnnn00011011","tas"),("0100mmmm00011110","ldc_gbr"),
 ("0100nnnn00100000","shal"),("0100nnnn00100001","shar"),("0100nnnn00100010","stsl_pr"),
 ("0100nnnn00100011","stcl_vbr"),("0100nnnn00100100","rotcl"),("0100nnnn00100101","rotcr"),
 ("0100mmmm00100110","ldsl_pr"),("0100mmmm00100111","ldcl_vbr"),("0100nnnn00101000","shll16"),
 ("0100nnnn00101001","shlr16"),("0100mmmm00101010","lds_pr"),("0100mmmm00101011","jmp"),
 ("0100mmmm00101110","ldc_vbr"),
 ("0100nnnn00110010","stcl_sgr"),("0100nnnn00110011","stcl_ssr"),("0100mmmm00110111","ldcl_ssr"),
 ("0100mmmm00111110","ldc_ssr"),("0100nnnn01000011","stcl_spc"),("0100mmmm01000111","ldcl_spc"),
 ("0100mmmm01001110","ldc_spc"),
 ("0100nnnn01010010","stsl_fpul"),("0100mmmm01010110","ldsl_fpul"),("0100mmmm01011010","lds_fpul"),
 ("0100nnnn01100010","stsl_fpscr"),("0100mmmm01100110","ldsl_fpscr"),("0100mmmm01101010","lds_fpscr"),
 ("0100nnnn11110010","stcl_dbr"),("0100mmmm11110110","ldcl_dbr"),("0100mmmm11111010","ldc_dbr"),
 ("0100nnnn1mmm0011","stcl_bank"),("0100mmmm1nnn0111","ldcl_bank"),("0100mmmm1nnn1110","ldc_bank"),
 ("0100nnnnmmmm1100","shad"),("0100nnnnmmmm1101","shld"),("0100nnnnmmmm1111","macw"),
 ("0101nnnnmmmmdddd","movl_l4"),
 # 0110
 ("0110nnnnmmmm0000","movb_l"),("0110nnnnmmmm0001","movw_l"),("0110nnnnmmmm0010","movl_l"),
 ("0110nnnnmmmm0011","mov"),("0110nnnnmmmm0100","movb_p"),("0110nnnnmmmm0101","movw_p"),
 ("0110nnnnmmmm0110","movl_p"),("0110nnnnmmmm0111","not"),("0110nnnnmmmm1000","swapb"),
 ("0110nnnnmmmm1001","swapw"),("0110nnnnmmmm1010","negc"),("0110nnnnmmmm1011","neg"),
 ("0110nnnnmmmm1100","extub"),("0110nnnnmmmm1101","extuw"),("0110nnnnmmmm1110","extsb"),
 ("0110nnnnmmmm1111","extsw"),
 ("0111nnnniiiiiiii","addi"),
 ("10000000nnnndddd","movb_s4"),("10000001nnnndddd","movw_s4"),
 ("10000100mmmmdddd","movb_l4"),("10000101mmmmdddd","movw_l4"),
 ("10001000iiiiiiii","cmpim"),("10001001dddddddd","bt"),("10001011dddddddd","bf"),
 ("10001101dddddddd","bts"),("10001111dddddddd","bfs"),
 ("1001nnnndddddddd","movw_pc"),
 ("1010dddddddddddd","bra"),("1011dddddddddddd","bsr"),
 ("11000000dddddddd","movb_sg"),("11000001dddddddd","movw_sg"),("11000010dddddddd","movl_sg"),
 ("11000011iiiiiiii","trapa"),
 ("11000100dddddddd","movb_lg"),("11000101dddddddd","movw_lg"),("11000110dddddddd","movl_lg"),
 ("11000111dddddddd","mova"),("11001000iiiiiiii","tsti"),("11001001iiiiiiii","andi"),
 ("11001010iiiiiiii","xori"),("11001011iiiiiiii","ori"),("11001100iiiiiiii","tstb"),
 ("11001101iiiiiiii","andb"),("11001110iiiiiiii","xorb"),("11001111iiiiiiii","orb"),
 ("1101nnnndddddddd","movl_pc"),("1110nnnniiiiiiii","movi"),
 # FPU
 ("1111nnnnmmmm0000","fadd"),("1111nnnnmmmm0001","fsub"),("1111nnnnmmmm0010","fmul"),
 ("1111nnnnmmmm0011","fdiv"),("1111nnnnmmmm0100","fcmpeq"),("1111nnnnmmmm0101","fcmpgt"),
 ("1111nnnnmmmm0110","fmov_l0"),("1111nnnnmmmm0111","fmov_s0"),("1111nnnnmmmm1000","fmov_l"),
 ("1111nnnnmmmm1001","fmov_p"),("1111nnnnmmmm1010","fmov_s"),("1111nnnnmmmm1011","fmov_m"),
 ("1111nnnnmmmm1100","fmov"),("1111nnnnmmmm1110","fmac"),
 ("1111nnnn00001101","fsts"),("1111mmmm00011101","flds"),("1111nnnn00101101","float"),
 ("1111mmmm00111101","ftrc"),("1111nnnn01001101","fneg"),("1111nnnn01011101","fabs"),
 ("1111nnnn01101101","fsqrt"),("1111nnnn01111101","fsrra"),("1111nnnn10001101","fldi0"),
 ("1111nnnn10011101","fldi1"),("1111nnnn10101101","fcnvsd"),("1111mmmm10111101","fcnvds"),
 ("1111nnmm11101101","fipr"),("1111nn0111111101","ftrv"),("1111nnn011111101","fsca"),
]

class Insn:
    __slots__=("op","n","m","imm","disp","word")
    def __init__(s,op,n,m,imm,disp,word): s.op,s.n,s.m,s.imm,s.disp,s.word=op,n,m,imm,disp,word
    def __repr__(s): return "%s n=%s m=%s i=%s d=%s"%(s.op,s.n,s.m,s.imm,s.disp)

def _compile():
    pats=[]
    for p,op in TABLE:
        assert len(p)==16,(p,op)
        mask=val=0; fields={}
        for i,c in enumerate(p):
            bit=15-i
            if c in "01": mask|=1<<bit; val|=int(c)<<bit
            else: fields.setdefault(c,[]).append(bit)
        pats.append((mask,val,op,{k:(min(v),len(v)) for k,v in fields.items()}))
    # more specific patterns first (more fixed bits)
    pats.sort(key=lambda t:-bin(t[0]).count("1"))
    lut=[None]*65536
    for w in range(65536):
        for mask,val,op,f in pats:
            if w&mask==val:
                g=lambda k:((w>>f[k][0])&((1<<f[k][1])-1)) if k in f else None
                lut[w]=(op,g("n"),g("m"),g("i"),g("d")); break
    return lut
_LUT=_compile()

def decode(w):
    e=_LUT[w]
    return None if e is None else Insn(e[0],e[1],e[2],e[3],e[4],w)

DELAY = {"bra","bsr","braf","bsrf","jmp","jsr","rts","rte","bts","bfs"}
# instructions illegal in a delay slot (slot illegal instruction)
NO_SLOT = DELAY|{"bt","bf","trapa","movw_pc","movl_pc","mova"} - {"movw_pc","movl_pc","mova"}

def s8(x): return x-256 if x&0x80 else x
def s12(x): return x-4096 if x&0x800 else x

def branch_target(ins,pc):
    if ins.op in ("bt","bf","bts","bfs"): return pc+4+s8(ins.disp)*2
    if ins.op in ("bra","bsr"): return pc+4+s12(ins.disp)*2
    return None

def pcrel_addr(ins,pc):
    if ins.op=="movl_pc" or ins.op=="mova": return ((pc+4)&~3)+ins.disp*4
    if ins.op=="movw_pc": return pc+4+ins.disp*2
    return None

class Image:
    def __init__(s,path,base=0x8c010000):
        s.data=open(path,"rb").read(); s.base=base; s.end=base+len(s.data)
    def has(s,a,n=1): return s.base<=a and a+n<=s.end
    def u16(s,a): return struct.unpack_from("<H",s.data,a-s.base)[0]
    def u32(s,a): return struct.unpack_from("<I",s.data,a-s.base)[0]
    def s16(s,a): v=s.u16(a); return v-65536 if v&0x8000 else v
