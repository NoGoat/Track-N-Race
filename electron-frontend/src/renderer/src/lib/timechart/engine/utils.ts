type CbParameters<T extends (...args: Array<any>) => void> = T extends (...args: infer P) => void ? P : never;

export class EventDispatcher<TCb extends (...args: Array<any>) => void = (() => void)>  {
    private callbacks: Array<TCb> = []
    on(callback: TCb) {
        this.callbacks.push(callback);
        return () => {
            const index = this.callbacks.indexOf(callback);
            if (index !== -1) this.callbacks.splice(index, 1);
        };
    }
    dispatch(...args: CbParameters<TCb>) {
        for (const cb of this.callbacks) {
            cb(...args);
        }
    }
}
