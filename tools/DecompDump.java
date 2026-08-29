import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.*;
public class DecompDump extends GhidraScript {
    @Override public void run() throws Exception {
        long[] rvas = {0x838290};
        String outPath = System.getenv("DECOMP_OUT");
        if(outPath==null) outPath="decomp_out.txt";
        PrintWriter pw=new PrintWriter(new FileWriter(outPath));
        DecompInterface di=new DecompInterface(); di.openProgram(currentProgram);
        for(long rva:rvas){
            Address a=currentProgram.getImageBase().add(rva);
            Function f=getFunctionContaining(a); if(f==null)f=createFunction(a,null);
            pw.println("==== RVA 0x"+Long.toHexString(rva)+" "+(f!=null?f.getName():"?")+" ====");
            if(f!=null){ DecompileResults r=di.decompileFunction(f,90,monitor);
                pw.println(r!=null&&r.decompileCompleted()?r.getDecompiledFunction().getC():"(fail)"); }
            pw.flush();
        }
        pw.close(); println("done");
    }
}
