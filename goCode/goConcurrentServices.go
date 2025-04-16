package main

import (
	"fmt"
	"log"
	"math/rand"
	"strings"
	"sync"
	"time"

	"github.com/hashicorp/consul/api"
)

//#include <stdlib.h>
import "C"

func main() {
}

var (
	client           *api.Client                 // consul client
	lock             sync.RWMutex                // mutex used for locking
	serviceAddresses = make(map[*C.char]*C.char) // saves the addresses to a map in memory
	addressReady     = sync.NewCond(&lock)       // used to populate initial values
)

func initConsul() {
	consulClient, err := api.NewClient(&api.Config{
		Address: "127.0.0.1:8500",
	})
	if err != nil {
		log.Fatal(err)
	}
	client = consulClient
}

// starts the goroutine to fetch addresses, also waits for the initial values to be populated
//
//export GetConsulAddresses
func GetConsulAddresses(services *C.char) C.int {
	if client == nil {
		initConsul()
	}
	//time.Sleep(100 * time.Millisecond)
	serviceAddresses[services] = C.CString("")
	go RefreshAddresses(services, serviceAddresses)
	//Block until serviceAddress is populated
	lock.Lock()
	for serviceAddresses[services] == nil {
		log.Println("Waiting for service address to be ready...")
		addressReady.Wait()
	}
	lock.Unlock()
	log.Println("Initial service address is ready!")

	return C.int(1)
}

// constantly refreshed the healthy addresses
func RefreshAddresses(services *C.char, serviceAddresses map[*C.char]*C.char) {
	lock.Lock()
	service, tag := convertToGoString(C.GoString(services))
	log.Printf("[debug] consul: lookup service=%s, tag=%s", service, tag)
	list, err := consulServices(service, tag)
	if err != nil {
		log.Printf("[error] consul lookup failed: %v", err)
		log.Fatal(err)
	}

	if len(list) < 1 {
		serviceAddresses[services] = C.CString("")
		addressReady.Broadcast()
		lock.Unlock()
	}
	// for now a random healthy address is selected from the list
	i := rand.Intn(len(list))
	serviceAddresses[services] = C.CString(list[i])
	lock.Unlock()
	addressReady.Broadcast()
	time.Sleep(time.Second)

	// new ticker is started to refresh the address every 10 seconds
	ticker := time.NewTicker(time.Second * 10)
	log.Println("Starting consul service loop...")
	for {
		select {
		case <-ticker.C:
			lock.Lock()
			log.Printf("[debug] consul: lookup service=%s, tag=%s", service, tag)
			list, err := consulServices(service, tag)
			if err != nil {
				log.Printf("[error] consul lookup failed: %v", err)
				log.Fatal(err)
			}
			if len(list) < 1 {
				serviceAddresses[services] = C.CString("")
				lock.Unlock()
				continue
			}
			i := rand.Intn(len(list))

			log.Printf("[debug] consul: returned %d services", len(list))
			log.Printf("[debug] consul: returned %s ", list[i])
			serviceAddresses[services] = C.CString(list[i])
			lock.Unlock()

			log.Printf("[debug] consul: new value of the serviceAddress %s ", C.GoString(serviceAddresses[services]))
		}

	}
}

// returns the healthy address saved in memory
//
//export ReturnAddress
func ReturnAddress(service *C.char) *C.char {
	lock.Lock()
	defer lock.Unlock()

	if serviceAddresses[service] == nil {
		log.Println("[ReturnAddress] serviceAddress is nil")
		return C.CString("")
	}

	return serviceAddresses[service]
}

// Converts the C string into two Golang strings: service and tag
func convertToGoString(s string) (service, tag string) {
	split := strings.SplitN(s, ".", 2)

	switch {
	case len(split) == 0:
		log.Fatal("No arguments supplied")
	case len(split) == 1:
		service = split[0]
		tag = ""
	default:
		service, tag = split[0], split[1]
	}

	return service, tag
}

// fetches the healthy address from consul
func consulServices(name string, tag string) ([]string, error) {
	if client == nil {
		log.Fatal("failed to initialize consul client")
	}
	services, _, err := client.Health().Service(name, tag, true, &api.QueryOptions{})
	if err != nil {
		addrs := make([]string, 0)
		return addrs, fmt.Errorf("failed to lookup service %q: %s", name, err)
	}

	addrs := make([]string, len(services))
	for i, s := range services {
		addr := s.Service.Address
		if addr == "" {
			addr = s.Node.Address
		}
		addrs[i] = fmt.Sprintf("%s:%d", addr, s.Service.Port+1)
		log.Printf("[debug] consul: returned service with address:%s and port:%d", addr, s.Service.Port+1)
	}

	return addrs, nil
}
