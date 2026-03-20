package goldengate.tests.models

import chisel3._
import chisel3.util.{Decoupled, log2Ceil}
import chiseltest._
import junctions.{NastiKey, NastiParameters}
import midas.models.ReadEgress
import midas.widgets.MultiQueue
import org.chipsalliance.cde.config.Parameters
import org.scalatest.flatspec.AnyFlatSpec
import org.scalatest.matchers.should.Matchers

class ReadEgressDebugHarness(
  maxRequests: Int,
  maxReqLength: Int,
  maxReqsPerId: Int,
)(implicit p: Parameters)
    extends ReadEgress(maxRequests, maxReqLength, maxReqsPerId) {
  private val idBits = p(NastiKey).idBits
  private val pIdBits = log2Ceil(maxRequests)

  val debug = IO(new Bundle {
    val debugCurrReqValid = Output(Bool())
    val debugCurrReqBits = Output(UInt(idBits.W))
    val debugDeqPIdRegValid = Output(Bool())
    val debugDeqPIdRegBits = Output(UInt(pIdBits.W))
    val debugLiveDeqAddr = Output(UInt(pIdBits.W))
    val debugRegDeqAddr = Output(UInt(pIdBits.W))
    val debugDeqData = Output(UInt(p(NastiKey).dataBits.W))
  })

  require(generateTranslation, "ReadEgressSpec requires the translation path to be enabled")
  private val shadowDeqAddrReg = RegNext(multiQueue.io.deqAddr)

  debug.debugCurrReqValid := currReqReg.valid
  debug.debugCurrReqBits := currReqReg.bits
  debug.debugDeqPIdRegValid := deqPIdReg.valid
  debug.debugDeqPIdRegBits := deqPIdReg.bits
  debug.debugLiveDeqAddr := multiQueue.io.deqAddr
  debug.debugRegDeqAddr := shadowDeqAddrReg
  debug.debugDeqData := multiQueue.io.deq.bits.data
}

class MultiQueueDebugHarness extends Module {
  val io = IO(new Bundle {
    val enq = Flipped(Decoupled(UInt(32.W)))
    val enqAddr = Input(UInt(1.W))
    val deqAddr = Input(UInt(1.W))
    val deqReady = Input(Bool())

    val deqValid = Output(Bool())
    val deqData = Output(UInt(32.W))
    val debugPrevDeqAddr = Output(UInt(1.W))
  })

  val dut = Module(new MultiQueue(UInt(32.W), numQueues = 2, requestedEntries = 2))
  val prevDeqAddr = RegNext(io.deqAddr, 0.U)

  dut.io.enq <> io.enq
  dut.io.enqAddr := io.enqAddr
  dut.io.deqAddr := io.deqAddr
  dut.io.deq.ready := io.deqReady

  io.deqValid := dut.io.deq.valid
  io.deqData := dut.io.deq.bits
  io.debugPrevDeqAddr := prevDeqAddr
}

class SingleEntryMultiQueueDebugHarness extends Module {
  private val numQueues = 16
  private val queueBits = log2Ceil(numQueues)

  val io = IO(new Bundle {
    val enq = Flipped(Decoupled(UInt(32.W)))
    val enqAddr = Input(UInt(queueBits.W))
    val deqAddr = Input(UInt(queueBits.W))
    val deqReady = Input(Bool())

    val deqValid = Output(Bool())
    val deqData = Output(UInt(32.W))
  })

  val dut = Module(new MultiQueue(UInt(32.W), numQueues = numQueues, requestedEntries = 1))

  dut.io.enq <> io.enq
  dut.io.enqAddr := io.enqAddr
  dut.io.deqAddr := io.deqAddr
  dut.io.deq.ready := io.deqReady

  io.deqValid := dut.io.deq.valid
  io.deqData := dut.io.deq.bits
}

class ReadEgressSpec extends AnyFlatSpec with ChiselScalatestTester with Matchers {
  private implicit val p: Parameters = Parameters.empty.alterPartial {
    case NastiKey => NastiParameters(dataBits = 32, addrBits = 32, idBits = 7)
  }

  private val MaxRequests = 16
  private val MaxReqLength = 1
  private val MaxReqsPerId = 40

  private val ReqA = 0x31
  private val ReqB = 0x24
  private val DataA = BigInt("11111111", 16)
  private val DataB = BigInt("b200b200", 16)
  private val AliasQueueA = 9
  private val AliasQueueB = 1
  private val AliasDataA = BigInt("a2a2a2a2", 16)
  private val AliasDataB = BigInt("dddddddd", 16)

  private def setDefaults(c: ReadEgressDebugHarness): Unit = {
    c.io.enq.valid.poke(false.B)
    c.io.enq.bits.id.poke(0.U)
    c.io.enq.bits.data.poke(0.U)
    c.io.enq.bits.last.poke(true.B)
    c.io.enq.bits.resp.poke(0.U)
    c.io.enq.bits.user.poke(0.U)

    c.io.req.hValid.poke(false.B)
    c.io.req.t.valid.poke(false.B)
    c.io.req.t.bits.poke(0.U)

    c.io.resp.tReady.poke(true.B)
  }

  private def enqueueResponse(c: ReadEgressDebugHarness, id: Int, data: BigInt): Unit = {
    c.io.enq.ready.expect(true.B)
    c.io.enq.valid.poke(true.B)
    c.io.enq.bits.id.poke(id.U)
    c.io.enq.bits.data.poke(data.U)
    c.io.enq.bits.last.poke(true.B)
    c.io.enq.bits.resp.poke(0.U)
    c.io.enq.bits.user.poke(0.U)
    c.clock.step()
    c.io.enq.valid.poke(false.B)
  }

  private def presentRequest(c: ReadEgressDebugHarness, id: Int): Unit = {
    c.io.req.hValid.poke(true.B)
    c.io.req.t.valid.poke(true.B)
    c.io.req.t.bits.poke(id.U)
  }

  "ReadEgress" should "keep MultiQueue dequeue address pinned to the active request until retirement" in {
    test(new ReadEgressDebugHarness(MaxRequests, MaxReqLength, MaxReqsPerId)) { c =>
      setDefaults(c)
      c.clock.step()

      enqueueResponse(c, ReqA, DataA)
      enqueueResponse(c, ReqB, DataB)

      presentRequest(c, ReqA)
      c.clock.step()

      presentRequest(c, ReqB)

      c.debug.debugCurrReqValid.expect(true.B)
      c.debug.debugCurrReqBits.expect(ReqA.U)
      c.debug.debugDeqPIdRegValid.expect(true.B)
      c.io.resp.hValid.expect(true.B)
      c.io.resp.tBits.id.expect(ReqA.U)

      c.debug.debugLiveDeqAddr.peek().litValue should equal(c.debug.debugRegDeqAddr.peek().litValue)
    }
  }

  private def setMultiQueueDefaults(c: MultiQueueDebugHarness): Unit = {
    c.io.enq.valid.poke(false.B)
    c.io.enq.bits.poke(0.U)
    c.io.enqAddr.poke(0.U)
    c.io.deqAddr.poke(0.U)
    c.io.deqReady.poke(false.B)
  }

  private def enqueueQueue(c: MultiQueueDebugHarness, addr: Int, data: BigInt): Unit = {
    c.io.enq.valid.poke(true.B)
    c.io.enq.bits.poke(data.U)
    c.io.enqAddr.poke(addr.U)
    c.clock.step()
    c.io.enq.valid.poke(false.B)
  }

  private def setSingleEntryMultiQueueDefaults(c: SingleEntryMultiQueueDebugHarness): Unit = {
    c.io.enq.valid.poke(false.B)
    c.io.enq.bits.poke(0.U)
    c.io.enqAddr.poke(0.U)
    c.io.deqAddr.poke(0.U)
    c.io.deqReady.poke(false.B)
  }

  private def enqueueSingleEntryQueue(c: SingleEntryMultiQueueDebugHarness, addr: Int, data: BigInt): Unit = {
    c.io.enq.valid.poke(true.B)
    c.io.enq.bits.poke(data.U)
    c.io.enqAddr.poke(addr.U)
    c.clock.step()
    c.io.enq.valid.poke(false.B)
  }

  "MultiQueue" should "show dequeue-address skew if the reader switches queues mid-stream" in {
    test(new MultiQueueDebugHarness) { c =>
      setMultiQueueDefaults(c)
      c.clock.step()

      enqueueQueue(c, addr = 0, data = DataA)
      enqueueQueue(c, addr = 1, data = DataB)

      c.io.deqAddr.poke(0.U)
      c.io.deqReady.poke(false.B)
      c.clock.step()

      c.io.deqAddr.poke(1.U)
      c.io.deqReady.poke(true.B)

      c.io.deqValid.expect(true.B)
      c.io.debugPrevDeqAddr.expect(0.U)
      c.io.deqAddr.peek().litValue should equal(1)
    }
  }

  "MultiQueue" should "keep different queues isolated when configured for one entry per queue" in {
    test(new SingleEntryMultiQueueDebugHarness) { c =>
      setSingleEntryMultiQueueDefaults(c)
      c.clock.step()

      enqueueSingleEntryQueue(c, addr = AliasQueueA, data = AliasDataA)
      enqueueSingleEntryQueue(c, addr = AliasQueueB, data = AliasDataB)

      c.io.deqAddr.poke(AliasQueueA.U)
      c.io.deqReady.poke(false.B)
      c.clock.step()

      c.io.deqReady.poke(true.B)
      c.io.deqValid.expect(true.B)
      c.io.deqData.expect(AliasDataA.U)
    }
  }
}
