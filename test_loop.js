const n = 2000000;
const arr = new Uint8Array(n);
arr[0] = 8;
const start = performance.now();
let neededMask = (1 << 8);
let c = 0;
for (let i = n - 1; i >= 0 && neededMask !== 0; i--) {
  const t = arr[i];
  if (t >= 6 && t <= 10) {
    if ((neededMask & (1 << t)) !== 0) {
      neededMask &= ~(1 << t);
    }
  }
  c++;
}
const end = performance.now();
console.log(`Time: ${end - start} ms, iterations: ${c}`);
