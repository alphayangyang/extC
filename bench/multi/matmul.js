const n = 512, N = n*n, a = new Int32Array(N), b = new Int32Array(N), c = new Int32Array(N);
for (let i = 0; i < N; i++) { a[i] = 1; b[i] = 2; }
for (let x = 0; x < n; x++) for (let y = 0; y < n; y++) {
  let s = 0; for (let k = 0; k < n; k++) s += a[x*n+k] * b[k*n+y];
  c[x*n+y] = s;
}
console.log("c[0] =", c[0]);
