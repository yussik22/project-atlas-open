int main() {
    volatile int counter = 7;
    counter += 5;
    return counter == 12 ? 0 : 1;
}
