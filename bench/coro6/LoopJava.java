public class LoopJava {
  static long run(long k, long m){ long s=0;
    for(long id=0; id<k; id++) for(long i=0;i<m;i++) s += (i%65536)*65521 + id*40503;
    return s; }
  public static void main(String[] a){ long t=run(8,2000); for(int r=0;r<1;r++) t+=run(512,20000);
    System.out.println(t); }
}
